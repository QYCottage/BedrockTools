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
    // Configuration callbacks arrive through the launcher's JNI bridge, so the
    // current thread is already attached to the VM. Do not attach arbitrary
    // game threads here: the overlay manager must be used on the Android UI
    // thread, and this function is intentionally a no-op outside that path.
    JavaVM* vm = gJavaVm.load(std::memory_order_acquire);
    if (!vm) return;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return;

    constexpr const char* managerClassName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager";
    jclass managerClass = env->FindClass(managerClassName);
    if (!managerClass || env->ExceptionCheck()) {
        clearJavaException(env);
        return;
    }

    jmethodID getInstance = env->GetStaticMethodID(
        managerClass, "getInstance", "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
    if (!getInstance || env->ExceptionCheck()) {
        clearJavaException(env);
        env->DeleteLocalRef(managerClass);
        return;
    }

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    if (!manager || env->ExceptionCheck()) {
        clearJavaException(env);
        env->DeleteLocalRef(managerClass);
        return;
    }

    jmethodID handleModuleToggle = env->GetMethodID(
        managerClass, "handleExternalModuleToggle", "(Ljava/lang/String;Z)V");
    if (!handleModuleToggle || env->ExceptionCheck()) {
        clearJavaException(env);
        env->DeleteLocalRef(manager);
        env->DeleteLocalRef(managerClass);
        return;
    }

    const std::string moduleIdString(moduleId);
    jstring javaModuleId = env->NewStringUTF(moduleIdString.c_str());
    if (!javaModuleId || env->ExceptionCheck()) {
        clearJavaException(env);
        env->DeleteLocalRef(manager);
        env->DeleteLocalRef(managerClass);
        return;
    }

    // Re-registering a button updates the native registry, but the launcher
    // overlay stores the old ButtonInfo object. Reusing the launcher's normal
    // hide/show path makes it fetch the freshly registered button definitions
    // while preserving each button's position (the IDs stay stable).
    env->CallVoidMethod(manager, handleModuleToggle, javaModuleId, JNI_FALSE);
    clearJavaException(env);
    env->CallVoidMethod(manager, handleModuleToggle, javaModuleId, JNI_TRUE);
    clearJavaException(env);

    env->DeleteLocalRef(javaModuleId);
    env->DeleteLocalRef(manager);
    env->DeleteLocalRef(managerClass);
#endif
}

} // namespace bedrocktools::launcher
