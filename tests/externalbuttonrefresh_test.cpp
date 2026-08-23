// Unit tests for the external overlay button refresh (Android JNI path).
//
// Regression tests for the bug where editing a Command Hotkey command/label
// or a Comment Key text (or a button size/color) never showed up on the
// already visible overlay button until the module was removed and re-added:
//
//   * the ModMenu config callback can run on a native thread that is NOT
//     attached to the JVM. The refresh must AttachCurrentThread for the
//     duration of the update and DetachCurrentThread afterwards (leaving the
//     caller's thread state as it found it) instead of silently doing
//     nothing, which is what made the stale button survive every edit.
//   * the already-created Java overlay button is updated in place: the fresh
//     button definition is swapped onto the overlay, applyConfigurationChanges
//     re-applies size/colors, and configureOverlayView re-writes the label
//     text on the existing view.
//   * buttons owned by other modules are left untouched.
//   * a thread that is already attached is used as-is (no attach/detach pair).
//   * the attach is not leaked when the launcher classes cannot be found.
//
// The launcher classes are faked through tests/fakejni/jni.h hooks, so the
// production ExternalButtonRefresh.cpp translation unit is compiled and
// exercised directly on the host.
//
// Build and run standalone:
//     g++ -std=c++20 -Wall -Wextra -I src -I tests/fakejni \
//         tests/externalbuttonrefresh_test.cpp -o /tmp/externalbuttonrefresh_test
//     /tmp/externalbuttonrefresh_test

#include "fakejni/jni.h"

// The implementation under test is compiled for the Android JNI path.
#define __ANDROID__ 1
#include "launcher/ExternalButtonRefresh.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

// ---------------------------------------------------------------------------
// Fake launcher object model
// ---------------------------------------------------------------------------

struct FakeClass;

struct FakeObject : _jobject {
    std::string kind; // "manager" | "map" | "button" | "overlay" | "view" | "string"
    std::string text; // payload for strings / button ids

    // Per-overlay records (what the real ExternalButtonOverlay would render).
    jobject currentButton = nullptr; // value of its "button" field
    int applyChangesCalls = 0;       // applyConfigurationChanges() invocations
    int configureCalls = 0;          // configureOverlayView(view) invocations
    jobject configureViewArg = nullptr;

    FakeObject() = default;
    FakeObject(std::string k, std::string t = {}) : kind(std::move(k)), text(std::move(t)) {}
};

struct FakeClass : _jclass {
    std::string name;
    FakeClass* super = nullptr;
    std::vector<std::string> fields; // fields declared on this exact class

    FakeClass(std::string n, FakeClass* s, std::vector<std::string> f)
        : name(std::move(n)), super(s), fields(std::move(f)) {}
};

struct FakeMethod : _jmethodID {
    const char* name;
    explicit FakeMethod(const char* n) : name(n) {}
};
struct FakeField : _jfieldID {
    const char* name;
    explicit FakeField(const char* n) : name(n) {}
};

// Launcher classes. Note that "overlayView" only exists on the *base* class so
// the tests exercise the superclass walk in the field lookup.
FakeClass gManagerClass{"InbuiltOverlayManager", nullptr, {"externalButtonOverlayMap"}};
FakeClass gBridgeClass{"ExternalModBridge", nullptr, {}};
FakeClass gButtonClass{"ExternalButton", nullptr, {"buttonId", "moduleId"}};
FakeClass gOverlayBase{"ExternalOverlayBase", nullptr, {"overlayView"}};
FakeClass gOverlayClass{"ExternalButtonOverlay", &gOverlayBase, {"button"}};
FakeClass gMapClass{"java/util/Map", nullptr, {}};

FakeMethod mGetInstance{"getInstance"};
FakeMethod mGetCount{"getExternalButtonCount"};
FakeMethod mGetButton{"getExternalButton"};
FakeMethod mMapGet{"get"};
FakeMethod mApplyChanges{"applyConfigurationChanges"};
FakeMethod mConfigure{"configureOverlayView"};

FakeField fOverlays{"externalButtonOverlayMap"};
FakeField fButtonId{"buttonId"};
FakeField fModuleId{"moduleId"};
FakeField fOverlayButton{"button"};
FakeField fOverlayView{"overlayView"};

// Instances: two buttons registered in the bridge, one for Command Hotkey and
// one owned by an unrelated module (Zoom), each with its own overlay.
FakeObject gManager{"manager"};
FakeObject gMap{"map"};
FakeObject gButtonStale{"button"};   // old definition still held by the overlay
FakeObject gButtonFresh{"button"};   // freshly registered Command Hotkey button
FakeObject gButtonZoom{"button"};    // belongs to another module
FakeObject gOverlay{"overlay"};
FakeObject gOverlayZoom{"overlay"};
FakeObject gView{"view"};
FakeObject gViewZoom{"view"};
FakeObject gModuleStr{"string", "bedrocktools.Command Hotkey"};
FakeObject gZoomModuleStr{"string", "bedrocktools.Zoom"};
FakeObject gButtonIdStr{"string", "bedrocktools.CommandHotkey.Button1"};
FakeObject gZoomButtonIdStr{"string", "bedrocktools.Zoom.Button1"};

// VM / call bookkeeping.
int g_attachCalls = 0;
int g_detachCalls = 0;
int g_attachResult = JNI_OK;
int g_getSuperclassCalls = 0;
bool g_classesReachable = true;
std::vector<std::string> g_log; // ordered events: "attach", "detach", "findclass:..."

FakeObject* asFake(jobject o) { return static_cast<FakeObject*>(o); }

// ---------------------------------------------------------------------------
// JNIEnv hooks: a miniature scripted InbuiltOverlayManager / ExternalModBridge
// ---------------------------------------------------------------------------

jclass hk_FindClass(JNIEnv*, const char* name) {
    g_log.push_back(std::string("findclass:") + name);
    if (!g_classesReachable) return nullptr;
    if (std::strstr(name, "InbuiltOverlayManager")) return &gManagerClass;
    if (std::strstr(name, "ExternalModBridge$ExternalButton")) return &gButtonClass;
    if (std::strstr(name, "ExternalButtonOverlay")) return &gOverlayClass;
    if (std::strstr(name, "ExternalModBridge")) return &gBridgeClass;
    if (std::strcmp(name, "java/util/Map") == 0) return &gMapClass;
    return nullptr;
}

jmethodID hk_GetStaticMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gManagerClass && std::strcmp(name, "getInstance") == 0) return &mGetInstance;
    if (c == &gBridgeClass && std::strcmp(name, "getExternalButtonCount") == 0) return &mGetCount;
    if (c == &gBridgeClass && std::strcmp(name, "getExternalButton") == 0) return &mGetButton;
    return nullptr;
}

jmethodID hk_GetMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gMapClass && std::strcmp(name, "get") == 0) return &mMapGet;
    if (c == &gOverlayClass && std::strcmp(name, "applyConfigurationChanges") == 0) return &mApplyChanges;
    if (c == &gOverlayClass && std::strcmp(name, "configureOverlayView") == 0) return &mConfigure;
    return nullptr;
}

jfieldID hk_GetFieldID(JNIEnv*, jclass c, const char* name, const char*) {
    const FakeClass* fc = static_cast<FakeClass*>(c);
    for (const auto& f : fc->fields)
        if (f == name) {
            if (fc == &gManagerClass) return &fOverlays;
            if (fc == &gButtonClass)
                return std::strcmp(name, "buttonId") == 0 ? &fButtonId : &fModuleId;
            if (fc == &gOverlayClass) return &fOverlayButton;
            if (fc == &gOverlayBase) return &fOverlayView;
        }
    return nullptr; // field not declared on this class -> walk continues
}

jclass hk_GetSuperclass(JNIEnv*, jclass c) {
    ++g_getSuperclassCalls;
    return static_cast<FakeClass*>(c)->super;
}

jobject hk_NewLocalRef(JNIEnv*, jobject o) { return o; }

jobject hk_CallStaticObjectMethod(JNIEnv*, jclass c, jmethodID m, va_list ap) {
    if (m == &mGetInstance) return &gManager;
    if (m == &mGetButton) {
        const jint index = va_arg(ap, jint);
        return index == 0 ? &gButtonFresh : &gButtonZoom;
    }
    (void)c;
    return nullptr;
}

jint hk_CallStaticIntMethod(JNIEnv*, jclass, jmethodID m, va_list) {
    return m == &mGetCount ? 2 : 0;
}

jobject hk_CallObjectMethod(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    if (m == &mMapGet && o == &gMap) {
        const jobject key = va_arg(ap, jobject);
        if (key == &gButtonIdStr) return &gOverlay;
        if (key == &gZoomButtonIdStr) return &gOverlayZoom;
    }
    return nullptr;
}

void hk_CallVoidMethod(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    FakeObject* overlay = asFake(o);
    if (m == &mApplyChanges) {
        ++overlay->applyChangesCalls;
    } else if (m == &mConfigure) {
        ++overlay->configureCalls;
        overlay->configureViewArg = va_arg(ap, jobject);
    }
}

jobject hk_GetObjectField(JNIEnv*, jobject o, jfieldID f) {
    if (f == &fOverlays) return &gMap;
    if (f == &fModuleId)
        return o == &gButtonZoom ? &gZoomModuleStr : &gModuleStr;
    if (f == &fButtonId)
        return o == &gButtonZoom ? &gZoomButtonIdStr : &gButtonIdStr;
    if (f == &fOverlayButton) return asFake(o)->currentButton;
    if (f == &fOverlayView) return o == &gOverlayZoom ? &gViewZoom : &gView;
    return nullptr;
}

void hk_SetObjectField(JNIEnv*, jobject o, jfieldID f, jobject v) {
    if (f == &fOverlayButton) asFake(o)->currentButton = v;
}

jstring hk_NewStringUTF(JNIEnv*, const char* s) {
    static std::vector<FakeObject*> pool;
    pool.push_back(new FakeObject{"string", s});
    return pool.back();
}

jsize hk_GetStringUTFLength(JNIEnv*, jstring s) {
    return static_cast<jsize>(asFake(s)->text.size());
}

const char* hk_GetStringUTFChars(JNIEnv*, jstring s, jboolean* isCopy) {
    if (isCopy) *isCopy = JNI_FALSE;
    return asFake(s)->text.c_str();
}

void hk_ReleaseStringUTFChars(JNIEnv*, jstring, const char*) {}
jboolean hk_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
void hk_ExceptionClear(JNIEnv*) {}
void hk_DeleteLocalRef(JNIEnv*, jobject) {}
jboolean hk_IsSameObject(JNIEnv*, jobject a, jobject b) { return a == b; }

// ---------------------------------------------------------------------------
// Fake JavaVM
// ---------------------------------------------------------------------------

enum class EnvMode { Detached, Attached, VersionError };

EnvMode g_envMode = EnvMode::Detached;
JNIEnv g_env;
JavaVM g_vm;

jint hk_GetEnv(JavaVM*, void** out, jint) {
    if (g_envMode == EnvMode::Detached) return JNI_EDETACHED;
    if (g_envMode == EnvMode::VersionError) return JNI_EVERSION;
    *reinterpret_cast<JNIEnv**>(out) = &g_env;
    return JNI_OK;
}

jint hk_AttachCurrentThread(JavaVM*, JNIEnv** out, void*) {
    ++g_attachCalls;
    g_log.push_back("attach");
    if (g_attachResult != JNI_OK) return g_attachResult;
    *out = &g_env;
    return JNI_OK;
}

jint hk_DetachCurrentThread(JavaVM*) {
    ++g_detachCalls;
    g_log.push_back("detach");
    return JNI_OK;
}

void installHooks() {
    JNIEnv& e = g_env;
    e.FindClassFn = hk_FindClass;
    e.GetStaticMethodIDFn = hk_GetStaticMethodID;
    e.GetMethodIDFn = hk_GetMethodID;
    e.GetFieldIDFn = hk_GetFieldID;
    e.GetSuperclassFn = hk_GetSuperclass;
    e.NewLocalRefFn = hk_NewLocalRef;
    e.CallStaticObjectMethodVFn = hk_CallStaticObjectMethod;
    e.CallStaticIntMethodVFn = hk_CallStaticIntMethod;
    e.CallObjectMethodVFn = hk_CallObjectMethod;
    e.CallVoidMethodVFn = hk_CallVoidMethod;
    e.GetObjectFieldFn = hk_GetObjectField;
    e.SetObjectFieldFn = hk_SetObjectField;
    e.NewStringUTFFn = hk_NewStringUTF;
    e.GetStringUTFLengthFn = hk_GetStringUTFLength;
    e.GetStringUTFCharsFn = hk_GetStringUTFChars;
    e.ReleaseStringUTFCharsFn = hk_ReleaseStringUTFChars;
    e.ExceptionCheckFn = hk_ExceptionCheck;
    e.ExceptionClearFn = hk_ExceptionClear;
    e.DeleteLocalRefFn = hk_DeleteLocalRef;
    e.IsSameObjectFn = hk_IsSameObject;

    g_vm.GetEnvFn = hk_GetEnv;
    g_vm.AttachCurrentThreadFn = hk_AttachCurrentThread;
    g_vm.DetachCurrentThreadFn = hk_DetachCurrentThread;
}

void resetWorld() {
    g_attachCalls = g_detachCalls = g_getSuperclassCalls = 0;
    g_attachResult = JNI_OK;
    g_classesReachable = true;
    g_log.clear();

    gOverlay.currentButton = &gButtonStale;
    gOverlay.applyChangesCalls = 0;
    gOverlay.configureCalls = 0;
    gOverlay.configureViewArg = nullptr;
    gOverlayZoom.currentButton = &gButtonZoom;
    gOverlayZoom.applyChangesCalls = 0;
    gOverlayZoom.configureCalls = 0;
    gOverlayZoom.configureViewArg = nullptr;
}

bool attachHappenedBeforeFirstJniCall() {
    for (const auto& event : g_log) {
        if (event == "attach") return true;
        if (event.rfind("findclass:", 0) == 0) return false;
    }
    return false;
}

} // namespace

int main() {
    installHooks();
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf("external button refresh (detached native thread - the reported bug)\n");
    resetWorld();
    g_envMode = EnvMode::Detached;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");

    check(g_attachCalls == 1, "detached thread is attached to the JVM exactly once");
    check(g_detachCalls == 1, "thread is detached again after the refresh");
    check(attachHappenedBeforeFirstJniCall(), "no JNI call happens before AttachCurrentThread");
    check(gOverlay.currentButton == &gButtonFresh,
          "overlay receives the freshly registered button definition");
    check(gOverlay.applyChangesCalls >= 1, "applyConfigurationChanges re-applies size/colors in place");
    check(gOverlay.configureCalls >= 1, "configureOverlayView re-applies the label text in place");
    check(gOverlay.configureViewArg == &gView, "configureOverlayView runs on the overlay's own view");
    check(g_getSuperclassCalls >= 1, "overlayView field found through the superclass walk");
    check(gOverlayZoom.applyChangesCalls == 0 && gOverlayZoom.configureCalls == 0,
          "other modules' overlays are left untouched");
    check(gOverlayZoom.currentButton == &gButtonZoom, "other modules' buttons are not swapped");

    std::printf("external button refresh (already-attached thread)\n");
    resetWorld();
    g_envMode = EnvMode::Attached;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");

    check(g_attachCalls == 0 && g_detachCalls == 0,
          "already-attached thread is reused without attach/detach");
    check(gOverlay.currentButton == &gButtonFresh, "button definition still swapped in");
    check(gOverlay.applyChangesCalls >= 1 && gOverlay.configureCalls >= 1,
          "size/colors and label text still re-applied");

    std::printf("external button refresh (failure modes)\n");
    resetWorld();
    g_envMode = EnvMode::VersionError;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");
    check(g_attachCalls == 0 && gOverlay.currentButton == &gButtonStale,
          "GetEnv failure leaves everything untouched");

    resetWorld();
    g_envMode = EnvMode::Detached;
    g_attachResult = JNI_ERR;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");
    check(g_attachCalls == 1 && g_detachCalls == 0,
          "failed AttachCurrentThread is not followed by DetachCurrentThread");
    check(g_log.empty() || g_log.back() != "detach", "no detach event after a failed attach");

    resetWorld();
    g_envMode = EnvMode::Detached;
    g_classesReachable = false; // launcher classes not found on this build
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");
    check(g_attachCalls == 1 && g_detachCalls == 1,
          "early return while attached still detaches (no leaked attachment)");

    resetWorld();
    bedrocktools::launcher::setJavaVm(nullptr);
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktools.Command Hotkey");
    check(g_attachCalls == 0 && g_detachCalls == 0,
          "no VM registered -> no-op without crashing");
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
