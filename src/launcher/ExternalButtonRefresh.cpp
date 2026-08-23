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
    // The overlay keeps a Java ExternalButton object, so changing the native
    // definition alone is not enough to update an already visible button.
    // Replace that object in-place and ask the overlay to re-apply its view
    // configuration. This avoids the old hide/show workaround (which made the
    // buttons disappear while a text field was being edited).
    JavaVM* vm = gJavaVm.load(std::memory_order_acquire);
    if (!vm) return;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return;

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

    jmethodID getInstance = env->GetStaticMethodID(
        managerClass, "getInstance",
        "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
    jmethodID getCount = env->GetStaticMethodID(bridgeClass, "getExternalButtonCount", "()I");
    jmethodID getButton = env->GetStaticMethodID(
        bridgeClass, "getExternalButton",
        "(I)Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jfieldID overlaysField = env->GetFieldID(managerClass, "externalButtonOverlayMap", "Ljava/util/Map;");
    jmethodID mapGet = env->GetMethodID(
        env->FindClass("java/util/Map"), "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    jfieldID buttonIdField = env->GetFieldID(buttonClass, "buttonId", "Ljava/lang/String;");
    jfieldID moduleIdField = env->GetFieldID(buttonClass, "moduleId", "Ljava/lang/String;");
    jfieldID overlayButtonField = env->GetFieldID(overlayClass, "button",
                                                    "Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jmethodID applyChanges = env->GetMethodID(overlayClass, "applyConfigurationChanges", "()V");
    if (!getInstance || !getCount || !getButton || !overlaysField || !mapGet ||
        !buttonIdField || !moduleIdField || !overlayButtonField || !applyChanges ||
        env->ExceptionCheck()) {
        clearJavaException(env);
        return;
    }

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    jobject overlays = manager ? env->GetObjectField(manager, overlaysField) : nullptr;
    if (!manager || !overlays || env->ExceptionCheck()) {
        clearJavaException(env);
        return;
    }

    const std::string moduleIdString(moduleId);
    jstring wantedModule = env->NewStringUTF(moduleIdString.c_str());
    const jint count = env->CallStaticIntMethod(bridgeClass, getCount);
    for (jint i = 0; i < count && !env->ExceptionCheck(); ++i) {
        jobject button = env->CallStaticObjectMethod(bridgeClass, getButton, i);
        if (!button) continue;
        jstring buttonModule = static_cast<jstring>(env->GetObjectField(button, moduleIdField));
        if (!buttonModule || env->IsSameObject(buttonModule, wantedModule)) {
            if (buttonModule && env->GetStringUTFLength(buttonModule) ==
                                    env->GetStringUTFLength(wantedModule)) {
                // IsSameObject only compares references; compare the strings
                // below for the normal case where they are different objects.
                const char* actual = env->GetStringUTFChars(buttonModule, nullptr);
                const bool matches = actual && moduleIdString == actual;
                if (actual) env->ReleaseStringUTFChars(buttonModule, actual);
                if (!matches) { env->DeleteLocalRef(button); continue; }
            } else if (!buttonModule) {
                env->DeleteLocalRef(button);
                continue;
            }
        } else {
            env->DeleteLocalRef(button);
            continue;
        }

        jstring buttonId = static_cast<jstring>(env->GetObjectField(button, buttonIdField));
        jobject overlay = buttonId ? env->CallObjectMethod(overlays, mapGet, buttonId) : nullptr;
        if (overlay) {
            env->SetObjectField(overlay, overlayButtonField, button);
            env->CallVoidMethod(overlay, applyChanges);
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
    env->DeleteLocalRef(buttonClass);
    env->DeleteLocalRef(overlayClass);
    env->DeleteLocalRef(bridgeClass);
    env->DeleteLocalRef(managerClass);
#endif
}

} // namespace bedrocktools::launcher
