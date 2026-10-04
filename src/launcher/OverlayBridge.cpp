#include "OverlayBridge.hpp"
#include "OverlayTaskDex.hpp"
#include "modules/ModuleRegistry.hpp"
#include <pl/Logger.hpp>
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <type_traits>

namespace bedrocktools::launcher {
namespace {

constexpr const char* moduleId = "bedrocktools.Freelook";
constexpr const char* buttonId = "bedrocktools.Freelook.Button";
constexpr const char* positionKey = "external_button:bedrocktools.Freelook.Button";
JavaVM* javaVm = nullptr;
std::atomic_bool ready{false};
std::atomic_bool queued{false};
std::atomic_bool visible{false};
std::atomic_bool reset{false};
std::mutex initializationMutex;
std::atomic<std::int64_t> nextPoll{0};

struct JavaApi {
    jclass manager = nullptr;
    jclass preferences = nullptr;
    jclass input = nullptr;
    jclass task = nullptr;
    jmethodID managerInstance = nullptr;
    jmethodID preferencesInstance = nullptr;
    jmethodID toggle = nullptr;
    jmethodID refreshVisibility = nullptr;
    jmethodID runOnUiThread = nullptr;
    jmethodID finishing = nullptr;
    jmethodID destroyed = nullptr;
    jmethodID resources = nullptr;
    jmethodID metrics = nullptr;
    jmethodID getX = nullptr;
    jmethodID getY = nullptr;
    jmethodID setPosition = nullptr;
    jmethodID resolveEnabled = nullptr;
    jmethodID mapGet = nullptr;
    jmethodID click = nullptr;
    jmethodID taskConstructor = nullptr;
    jmethodID hudOpen = nullptr;
    jmethodID menuOpen = nullptr;
    jmethodID pauseOpen = nullptr;
    jmethodID setPadding = nullptr;
    jmethodID setBackgroundColor = nullptr;
    jfieldID activity = nullptr;
    jfieldID buttons = nullptr;
    jfieldID editor = nullptr;
    jfieldID active = nullptr;
    jfieldID width = nullptr;
    jfieldID height = nullptr;
    jfieldID density = nullptr;
    jfieldID overlayView = nullptr;
    jfieldID iconVisible = nullptr;
} api;

struct JavaFailure {};

class Java {
private:
    template<class Function> auto checked(Function function) {
        if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
            function();
            if (mEnv->ExceptionCheck()) throw JavaFailure{};
        } else {
            auto result = function();
            if (mEnv->ExceptionCheck()) throw JavaFailure{};
            return result;
        }
    }
    JNIEnv* mEnv;
public:
    explicit Java(JNIEnv* env) : mEnv(env) {}
    template<class... Args> jobject object(jobject value, jmethodID method, Args... args) {
        return checked([&] { return mEnv->CallObjectMethod(value, method, args...); });
    }
    template<class... Args> jobject staticObject(jclass type, jmethodID method, Args... args) {
        return checked([&] { return mEnv->CallStaticObjectMethod(type, method, args...); });
    }
    template<class... Args> jboolean boolean(jobject value, jmethodID method, Args... args) {
        return checked([&] { return mEnv->CallBooleanMethod(value, method, args...); });
    }
    jboolean staticBoolean(jclass type, jmethodID method) {
        return checked([&] { return mEnv->CallStaticBooleanMethod(type, method); });
    }
    template<class... Args> jint integer(jobject value, jmethodID method, Args... args) {
        return checked([&] { return mEnv->CallIntMethod(value, method, args...); });
    }
    template<class... Args> void call(jobject value, jmethodID method, Args... args) {
        checked([&] { mEnv->CallVoidMethod(value, method, args...); });
    }
    template<class... Args> jobject create(jclass type, jmethodID constructor, Args... args) {
        return checked([&] { return mEnv->NewObject(type, constructor, args...); });
    }
};

class Environment {
public:
    JNIEnv* env = nullptr;
    explicit Environment(JavaVM* vm) : mVm(vm) {
        if (!vm) return;
        const auto status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
#if defined(__ANDROID__)
            mAttached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
#else
            mAttached = vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK;
#endif
            if (!mAttached) env = nullptr;
        } else if (status != JNI_OK) env = nullptr;
        if (env && env->PushLocalFrame(64) != JNI_OK) {
            env->ExceptionClear();
            env = nullptr;
        }
    }
    ~Environment() {
        if (env) {
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                pl::log::Logger::getOrCreate("BedrockTools").error("Freelook overlay bridge Java call failed");
            }
            env->PopLocalFrame(nullptr);
        }
        if (mAttached) mVm->DetachCurrentThread();
    }
private:
    JavaVM* mVm;
    bool mAttached = false;
};

void JNICALL runTask(JNIEnv* env, jobject) {
    auto* mod = ModuleRegistry::get().find(moduleId);
    if (!mod) {
        queued.store(false, std::memory_order_release);
        return;
    }
    if (env->PushLocalFrame(32) != JNI_OK) {
        env->ExceptionClear();
        queued.store(false, std::memory_order_release);
        return;
    }
    try {
        Java java(env);
        auto manager = java.staticObject(api.manager, api.managerInstance);
        if (manager) {
            auto activity = env->GetObjectField(manager, api.activity);
            if (activity && !java.boolean(activity, api.finishing) &&
                !java.boolean(activity, api.destroyed)) {
                auto preferences = java.staticObject(api.preferences, api.preferencesInstance, activity);
                auto module = env->NewStringUTF(moduleId);
                auto button = env->NewStringUTF(buttonId);
                auto position = env->NewStringUTF(positionKey);
                auto map = env->GetObjectField(manager, api.buttons);
                auto overlay = java.object(map, api.mapGet, button);
                const bool enabled = java.boolean(preferences, api.resolveEnabled,
                    module, static_cast<jboolean>(mod->masterEnabled));
                if (enabled != mod->masterEnabled) pl::modmenu::setModuleEnabled(moduleId, enabled);
                const bool show = visible.load(std::memory_order_acquire);
                if (!show) {
                    if (overlay) java.call(manager, api.toggle, module, JNI_FALSE);
                } else {
                    const auto missing = std::numeric_limits<jint>::min();
                    const auto x = java.integer(preferences, api.getX, position, missing);
                    const auto y = java.integer(preferences, api.getY, position, missing);
                    if (x == missing || y == missing) {
                        auto resources = java.object(activity, api.resources);
                        auto metrics = java.object(resources, api.metrics);
                        const auto width = env->GetIntField(metrics, api.width);
                        const auto height = env->GetIntField(metrics, api.height);
                        const auto density = env->GetFloatField(metrics, api.density);
                        const int size = static_cast<int>(52 * density);
                        const int defaultX = std::clamp(width / 2 + static_cast<int>(38 * density),
                            0, std::max(0, width - size));
                        const int defaultY = std::clamp(height / 2 - static_cast<int>(26 * density),
                            0, std::max(0, height - size));
                        java.call(preferences, api.setPosition, position,
                            x == missing ? defaultX : x, y == missing ? defaultY : y);
                        if (overlay) {
                            java.call(manager, api.toggle, module, JNI_FALSE);
                            overlay = nullptr;
                        }
                    }
                    if (!overlay) {
                        java.call(manager, api.toggle, module, JNI_TRUE);
                        java.call(manager, api.refreshVisibility);
                        overlay = java.object(map, api.mapGet, button);
                    }
                }
                const bool clear = reset.exchange(false, std::memory_order_acq_rel) ||
                    env->GetBooleanField(manager, api.editor) ||
                    !java.staticBoolean(api.input, api.hudOpen) ||
                    java.staticBoolean(api.input, api.menuOpen) ||
                    java.staticBoolean(api.input, api.pauseOpen);
                if (overlay && clear && env->GetBooleanField(overlay, api.active)) {
                    java.call(overlay, api.click);
                }
                if (overlay) {
                    auto view = env->GetObjectField(overlay, api.overlayView);
                    if (view) {
                        const bool hasIcon = env->GetBooleanField(overlay, api.iconVisible);
                        java.call(view, api.setPadding, 0, 0, 0, 0);
                        if (!hasIcon && !env->GetBooleanField(manager, api.editor)) {
                            const bool active = env->GetBooleanField(overlay, api.active);
                            java.call(view, api.setBackgroundColor,
                                static_cast<jint>(active ? 0xFFC6C6C6u : 0xFF8B8B8Bu));
                        }
                    }
                }
            }
        }
    } catch (const JavaFailure&) {
        env->ExceptionClear();
        pl::log::Logger::getOrCreate("BedrockTools").error("Freelook overlay refresh failed");
    }
    env->PopLocalFrame(nullptr);
    queued.store(false, std::memory_order_release);
}

void postTask() {
    if (!ready.load(std::memory_order_acquire) || queued.exchange(true, std::memory_order_acq_rel)) return;
    Environment frame(javaVm);
    auto* env = frame.env;
    if (!env) {
        queued.store(false, std::memory_order_release);
        return;
    }
    try {
        Java java(env);
        auto manager = java.staticObject(api.manager, api.managerInstance);
        if (!manager) {
            queued.store(false, std::memory_order_release);
            return;
        }
        auto activity = env->GetObjectField(manager, api.activity);
        auto task = java.create(api.task, api.taskConstructor);
        if (activity && task) java.call(activity, api.runOnUiThread, task);
        if (!activity || !task || env->ExceptionCheck()) queued.store(false, std::memory_order_release);
    } catch (const JavaFailure&) {
        queued.store(false, std::memory_order_release);
    }
}

}

void initializeOverlayBridge(JavaVM* vm) {
    std::lock_guard lock(initializationMutex);
    if (!vm || ready.load(std::memory_order_acquire)) return;
    javaVm = vm;
    Environment frame(vm);
    auto* env = frame.env;
    if (!env) return;
    try {
        Java java(env);
        auto manager = env->FindClass("org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager");
        if (!manager) return;
        auto task = env->FindClass("bedrocktools/OverlayTask");
        if (!task) {
            env->ExceptionClear();
            auto classClass = env->FindClass("java/lang/Class");
            auto getLoader = env->GetMethodID(classClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
            auto parent = java.object(manager, getLoader);
            auto loaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
            if (!loaderClass) return;
            auto constructor = env->GetMethodID(loaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
            auto buffer = env->NewDirectByteBuffer(const_cast<unsigned char*>(overlayTaskDex), sizeof(overlayTaskDex));
            auto loader = java.create(loaderClass, constructor, buffer, parent);
            auto loadClass = env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
            auto name = env->NewStringUTF("bedrocktools.OverlayTask");
            task = static_cast<jclass>(java.object(loader, loadClass, name));
            if (!task) return;
        }
        JNINativeMethod native{const_cast<char*>("run"), const_cast<char*>("()V"), reinterpret_cast<void*>(runTask)};
        if (env->RegisterNatives(task, &native, 1) != JNI_OK) return;
        auto preferences = env->FindClass("org/levimc/launcher/core/mods/inbuilt/manager/InbuiltModManager");
        auto input = env->FindClass("org/levimc/launcher/preloader/PreloaderInput");
        auto activity = env->FindClass("android/app/Activity");
        auto resources = env->FindClass("android/content/res/Resources");
        auto metrics = env->FindClass("android/util/DisplayMetrics");
        auto map = env->FindClass("java/util/Map");
        auto overlay = env->FindClass("org/levimc/launcher/core/mods/inbuilt/overlay/ExternalButtonOverlay");
        auto view = env->FindClass("android/view/View");
        if (!preferences || !input || !activity || !resources || !metrics || !map || !overlay || !view) return;
#define METHOD(member, type, name, signature) \
        api.member = env->GetMethodID(type, name, signature); if (!api.member) return
#define STATIC_METHOD(member, type, name, signature) \
        api.member = env->GetStaticMethodID(type, name, signature); if (!api.member) return
#define FIELD(member, type, name, signature) \
        api.member = env->GetFieldID(type, name, signature); if (!api.member) return
        STATIC_METHOD(managerInstance, manager, "getInstance", "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
        STATIC_METHOD(preferencesInstance, preferences, "getInstance", "(Landroid/content/Context;)Lorg/levimc/launcher/core/mods/inbuilt/manager/InbuiltModManager;");
        METHOD(toggle, manager, "handleExternalModuleToggle", "(Ljava/lang/String;Z)V");
        METHOD(refreshVisibility, manager, "refreshRuntimeVisibility", "()V");
        METHOD(runOnUiThread, activity, "runOnUiThread", "(Ljava/lang/Runnable;)V");
        METHOD(finishing, activity, "isFinishing", "()Z");
        METHOD(destroyed, activity, "isDestroyed", "()Z");
        METHOD(resources, activity, "getResources", "()Landroid/content/res/Resources;");
        METHOD(metrics, resources, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
        METHOD(getX, preferences, "getOverlayPositionX", "(Ljava/lang/String;I)I");
        METHOD(getY, preferences, "getOverlayPositionY", "(Ljava/lang/String;I)I");
        METHOD(setPosition, preferences, "setOverlayPosition", "(Ljava/lang/String;II)V");
        METHOD(resolveEnabled, preferences, "resolveExternalModuleEnabled", "(Ljava/lang/String;Z)Z");
        METHOD(mapGet, map, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        METHOD(click, overlay, "onButtonClick", "()V");
        METHOD(taskConstructor, task, "<init>", "()V");
        STATIC_METHOD(hudOpen, input, "isHudScreenOpen", "()Z");
        STATIC_METHOD(menuOpen, input, "isShowingMenu", "()Z");
        STATIC_METHOD(pauseOpen, input, "isPauseMenuOpen", "()Z");
        METHOD(setPadding, view, "setPadding", "(IIII)V");
        METHOD(setBackgroundColor, view, "setBackgroundColor", "(I)V");
        FIELD(activity, manager, "activity", "Landroid/app/Activity;");
        FIELD(buttons, manager, "externalButtonOverlayMap", "Ljava/util/Map;");
        FIELD(editor, manager, "hudEditorMode", "Z");
        FIELD(active, overlay, "active", "Z");
        FIELD(width, metrics, "widthPixels", "I");
        FIELD(height, metrics, "heightPixels", "I");
        FIELD(density, metrics, "density", "F");
        FIELD(overlayView, overlay, "overlayView", "Landroid/view/View;");
        FIELD(iconVisible, overlay, "iconVisible", "Z");
#undef METHOD
#undef STATIC_METHOD
#undef FIELD
        api.manager = static_cast<jclass>(env->NewGlobalRef(manager));
        api.preferences = static_cast<jclass>(env->NewGlobalRef(preferences));
        api.input = static_cast<jclass>(env->NewGlobalRef(input));
        api.task = static_cast<jclass>(env->NewGlobalRef(task));
        if (api.manager && api.preferences && api.input && api.task) ready.store(true, std::memory_order_release);
    } catch (const JavaFailure&) {}
}

void setFreelookOverlayVisible(bool value) {
    visible.store(value, std::memory_order_release);
    postTask();
}

void resetFreelookOverlay() {
    reset.store(true, std::memory_order_release);
    postTask();
}

void pollOverlayBridge() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto next = nextPoll.load(std::memory_order_acquire);
    if (now < next || !nextPoll.compare_exchange_strong(next, now + 500, std::memory_order_acq_rel)) return;
    postTask();
}

}
