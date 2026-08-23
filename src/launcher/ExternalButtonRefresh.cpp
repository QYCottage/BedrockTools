#include "ExternalButtonRefresh.hpp"

#include <atomic>
#include <string>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace bedrocktools::launcher {
namespace {

#if defined(__ANDROID__)

std::atomic<JavaVM*> gJavaVm{nullptr};

void clearJavaException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

// Runs the actual overlay refresh. The caller guarantees that env belongs to
// the current thread (attached if necessary), so every early return below is
// safe without extra thread bookkeeping.
void refreshExternalButtonsAttached(JNIEnv* env, std::string_view moduleId) {
    // The overlay keeps a Java ExternalButton object, so changing the native
    // definition alone is not enough to update an already visible button.
    // Replace that object in-place and ask the overlay to re-apply its view
    // configuration. This avoids the old hide/show workaround (which made the
    // buttons disappear while a text field was being edited).
    constexpr const char* managerName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager";
    constexpr const char* bridgeName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge";
    constexpr const char* buttonName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton";
    constexpr const char* overlayName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/ExternalButtonOverlay";

    jclass managerClass = env->FindClass(managerName);
    jclass bridgeClass = env->FindClass(bridgeName);
    jclass overlayClass = env->FindClass(overlayName);
    jclass buttonClass = env->FindClass(buttonName);
    if (!managerClass || !bridgeClass || !overlayClass || !buttonClass || env->ExceptionCheck()) {
        clearJavaException(env);
        return;
    }

    jclass mapClass = env->FindClass("java/util/Map");
    jmethodID getInstance = env->GetStaticMethodID(
        managerClass, "getInstance",
        "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
    jmethodID getCount = env->GetStaticMethodID(bridgeClass, "getExternalButtonCount", "()I");
    jmethodID getButton = env->GetStaticMethodID(
        bridgeClass, "getExternalButton",
        "(I)Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jfieldID overlaysField = env->GetFieldID(managerClass, "externalButtonOverlayMap", "Ljava/util/Map;");
    jmethodID mapGet = mapClass ? env->GetMethodID(
                                     mapClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;")
                                : nullptr;
    jfieldID buttonIdField = env->GetFieldID(buttonClass, "buttonId", "Ljava/lang/String;");
    jfieldID moduleIdField = env->GetFieldID(buttonClass, "moduleId", "Ljava/lang/String;");
    jfieldID overlayButtonField = env->GetFieldID(overlayClass, "button",
                                                    "Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jmethodID applyChanges = env->GetMethodID(overlayClass, "applyConfigurationChanges", "()V");
    if (!getInstance || !getCount || !getButton || !overlaysField || !mapGet ||
        !buttonIdField || !moduleIdField || !overlayButtonField || !applyChanges ||
        env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        return;
    }

    // Best-effort: applyConfigurationChanges resizes/recolors the existing
    // view but does not re-apply the label text on every launcher build (the
    // TextView text is only set in configureOverlayView at creation). Look up
    // the view field and the configure method so the new command/comment label
    // can be re-applied in place. These are optional: if absent on an older
    // launcher build, the resize/recolor path above still runs.
    jfieldID overlayViewField = nullptr;
    {
        // The field may live on ExternalButtonOverlay itself or on any
        // superclass depending on the launcher build, so walk the chain
        // instead of only probing the direct superclass.
        jclass walker = static_cast<jclass>(env->NewLocalRef(overlayClass));
        while (walker && !overlayViewField) {
            overlayViewField = env->GetFieldID(walker, "overlayView", "Landroid/view/View;");
            clearJavaException(env);
            if (overlayViewField) break;
            jclass next = env->GetSuperclass(walker);
            env->DeleteLocalRef(walker);
            walker = next;
        }
        if (walker) env->DeleteLocalRef(walker);
    }
    jmethodID configureOverlayView = env->GetMethodID(
        overlayClass, "configureOverlayView", "(Landroid/view/View;)V");
    clearJavaException(env);

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    jobject overlays = manager ? env->GetObjectField(manager, overlaysField) : nullptr;
    if (!manager || !overlays || env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        return;
    }

    const std::string moduleIdString(moduleId);
    jstring wantedModule = env->NewStringUTF(moduleIdString.c_str());
    const jint count = env->CallStaticIntMethod(bridgeClass, getCount);
    for (jint i = 0; i < count && !env->ExceptionCheck(); ++i) {
        jobject button = env->CallStaticObjectMethod(bridgeClass, getButton, i);
        if (!button) continue;
        jstring buttonModule = static_cast<jstring>(env->GetObjectField(button, moduleIdField));
        if (!buttonModule) {
            env->DeleteLocalRef(button);
            continue;
        }

        // IsSameObject only compares references, and the wanted module id was
        // created fresh above, so in practice the two jstrings are distinct
        // objects and the *contents* must be compared. Skipping the content
        // comparison here silently filtered out every button and left the
        // visible overlay stale until the module was re-added.
        bool moduleMatches = env->IsSameObject(buttonModule, wantedModule);
        if (!moduleMatches) {
            const char* actual = env->GetStringUTFChars(buttonModule, nullptr);
            moduleMatches = actual && moduleIdString == actual;
            if (actual) env->ReleaseStringUTFChars(buttonModule, actual);
        }
        if (!moduleMatches) {
            env->DeleteLocalRef(button);
            env->DeleteLocalRef(buttonModule);
            continue;
        }

        jstring buttonId = static_cast<jstring>(env->GetObjectField(button, buttonIdField));
        jobject overlay = buttonId ? env->CallObjectMethod(overlays, mapGet, buttonId) : nullptr;
        if (overlay) {
            // Swap in the freshly registered button definition, then re-apply
            // size/colors and the label text to the already visible view.
            env->SetObjectField(overlay, overlayButtonField, button);
            env->CallVoidMethod(overlay, applyChanges);

            // Re-run the view configuration so a changed command/comment label
            // is actually written to the label TextView. Safe to call on an
            // already-shown view; it re-finds the same child views and re-sets
            // their text/icon/colors. applyConfigurationChanges alone does not
            // re-apply the label text on every launcher build.
            if (overlayViewField && configureOverlayView) {
                jobject overlayView = env->GetObjectField(overlay, overlayViewField);
                if (overlayView) {
                    env->CallVoidMethod(overlay, configureOverlayView, overlayView);
                    env->DeleteLocalRef(overlayView);
                }
            }
        }
        if (buttonId) env->DeleteLocalRef(buttonId);
        if (buttonModule) env->DeleteLocalRef(buttonModule);
        env->DeleteLocalRef(button);
        if (overlay) env->DeleteLocalRef(overlay);
    }
    clearJavaException(env);
    if (wantedModule) env->DeleteLocalRef(wantedModule);
    env->DeleteLocalRef(overlays);
    env->DeleteLocalRef(manager);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(buttonClass);
    env->DeleteLocalRef(overlayClass);
    env->DeleteLocalRef(bridgeClass);
    env->DeleteLocalRef(managerClass);
}

#endif

} // namespace

void setJavaVm(void* javaVm) {
#if defined(__ANDROID__)
    gJavaVm.store(static_cast<JavaVM*>(javaVm), std::memory_order_release);
#else
    (void)javaVm;
#endif
}

void refreshExternalButtonsForModule(std::string_view moduleId) {
#if !defined(__ANDROID__)
    (void)moduleId;
#else
    JavaVM* vm = gJavaVm.load(std::memory_order_acquire);
    if (!vm) return;

    // The ModMenu config-change callback is not guaranteed to arrive on a
    // thread that is already attached to the JVM (it can be dispatched from a
    // native game/hook thread). GetEnv would report JNI_EDETACHED there and a
    // plain "return" would leave the visible overlay stale - the user would
    // have to remove and re-add the module to see the new text or size. Attach
    // such a thread for the duration of the refresh and detach it afterwards,
    // leaving the caller's thread state exactly as it was.
    JNIEnv* env = nullptr;
    const jint envState = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envState == JNI_OK && env) {
        refreshExternalButtonsAttached(env, moduleId);
        return;
    }
    if (envState != JNI_EDETACHED) return;

    JavaVMAttachArgs attachArgs{
        JNI_VERSION_1_6, const_cast<char*>("BedrockTools/ButtonRefresh"), nullptr};
    if (vm->AttachCurrentThread(&env, &attachArgs) != JNI_OK || !env) return;

    // Detaching also frees every local reference created above, so the
    // borrowed thread does not leak references into the next attach.
    refreshExternalButtonsAttached(env, moduleId);
    vm->DetachCurrentThread();
#endif
}

} // namespace bedrocktools::launcher
