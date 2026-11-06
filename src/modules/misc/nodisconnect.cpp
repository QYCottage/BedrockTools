#include "nodisconnect.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

namespace {
using bedrocktools::memory::SignatureId;
using AppCommandFn = void(*)(void*, std::int32_t);
using PlatformFn = void(*)(void*);
using TrimMemoryFn = void(*)(void*, std::int32_t);
using ShutdownFn = void(*)(void*, void*);
using LocalPlayerFn = void*(*)(void*);

constexpr std::int32_t initWindow = 1;
constexpr std::int32_t termWindow = 2;
constexpr std::int32_t lowMemory = 10;
constexpr std::int32_t startActivity = 11;
constexpr std::int32_t stopActivity = 15;
constexpr std::int32_t destroyActivity = 16;
constexpr std::int32_t trimUiHidden = 20;
constexpr auto backgroundTickInterval = std::chrono::milliseconds(50);

std::atomic_bool moduleActive{false};
std::atomic_bool runtimeActive{true};
std::atomic_bool fastResumeReady{false};
std::atomic_bool sessionActive{false};
std::atomic_bool memoryPressureRequested{false};
std::atomic_bool shutdownRequested{false};
std::atomic_bool throttleBackground{false};
std::atomic_bool fallbackConnection{false};
AppCommandFn appCommandOriginal = nullptr;
PlatformFn suspendOriginal = nullptr;
PlatformFn resumeOriginal = nullptr;
TrimMemoryFn trimMemoryOriginal = nullptr;
ShutdownFn shutdownOriginal = nullptr;
LocalPlayerFn getLocalPlayer = nullptr;
bool (*eduMultiplayerOriginal)(void*) = nullptr;
std::array<bedrocktools::hooks::Handle, 5> fastHooks{};
bedrocktools::hooks::Handle eduHook = nullptr;
bool initialized = false;

struct BackgroundState {
    void* app = nullptr;
    void* retainedPlatform = nullptr;
    bool stopped = false;
    bool windowAvailable = false;
    bool memoryLimited = false;
};

thread_local BackgroundState background;
thread_local bool retainOnStop = false;
thread_local bool keepConnection = false;
thread_local std::int32_t currentCommand = -1;
thread_local std::chrono::steady_clock::time_point nextBackgroundTick{};

bool active() {
    return moduleActive.load(std::memory_order_acquire)
        && runtimeActive.load(std::memory_order_acquire);
}

class CommandScope {
public:
    CommandScope(std::int32_t command, bool retain, bool keep)
        : mCommand(std::exchange(currentCommand, command)),
          mRetain(std::exchange(retainOnStop, retain)),
          mKeep(std::exchange(keepConnection, keep)) {}

    ~CommandScope() {
        currentCommand = mCommand;
        retainOnStop = mRetain;
        keepConnection = mKeep;
    }

private:
    std::int32_t mCommand;
    bool mRetain;
    bool mKeep;
};

bool eduMultiplayerDetour(void* instance) {
    if (active() && !shutdownRequested.load(std::memory_order_acquire)
        && (!fastResumeReady.load(std::memory_order_acquire) || keepConnection
            || fallbackConnection.load(std::memory_order_acquire))) return true;
    return eduMultiplayerOriginal ? eduMultiplayerOriginal(instance) : false;
}

void releaseRetainedResources(bool keep) {
    auto* platform = std::exchange(background.retainedPlatform, nullptr);
    throttleBackground.store(false, std::memory_order_release);
    if (!platform || !suspendOriginal) return;
    fallbackConnection.store(keep, std::memory_order_release);
    CommandScope scope{-1, false, keep};
    suspendOriginal(platform);
}

void suspendDetour(void* platform) {
    if (fastResumeReady.load(std::memory_order_acquire)
        && currentCommand == stopActivity && retainOnStop
        && !shutdownRequested.load(std::memory_order_acquire)) {
        background.retainedPlatform = platform;
        throttleBackground.store(background.stopped && !background.windowAvailable, std::memory_order_release);
        return;
    }
    if (background.retainedPlatform == platform) {
        background.retainedPlatform = nullptr;
        throttleBackground.store(false, std::memory_order_release);
    }
    if (suspendOriginal) suspendOriginal(platform);
}

void resumeDetour(void* platform) {
    if (currentCommand == startActivity && background.retainedPlatform == platform) {
        background.retainedPlatform = nullptr;
        throttleBackground.store(false, std::memory_order_release);
        return;
    }
    if (resumeOriginal) resumeOriginal(platform);
}

void appCommandDetour(void* app, std::int32_t command) {
    if (background.app != app) {
        background = {};
        background.app = app;
        throttleBackground.store(false, std::memory_order_release);
    }

    const bool closing = shutdownRequested.load(std::memory_order_acquire)
        || command == destroyActivity;
    const bool backgroundActive = active() && !closing;
    const bool keep = backgroundActive
        && sessionActive.load(std::memory_order_acquire);

    if (command == initWindow) background.windowAvailable = true;
    if (command == termWindow) background.windowAvailable = false;
    if (command == lowMemory && memoryPressureRequested.exchange(false, std::memory_order_acq_rel)) {
        background.memoryLimited = true;
        releaseRetainedResources(keep);
    }
    if (command == destroyActivity) {
        shutdownRequested.store(true, std::memory_order_release);
        background.retainedPlatform = nullptr;
        sessionActive.store(false, std::memory_order_release);
        fallbackConnection.store(false, std::memory_order_release);
    }
    if (command == stopActivity) background.stopped = true;
    if (command == startActivity) background.stopped = false;
    throttleBackground.store(background.retainedPlatform && background.stopped
        && !background.windowAvailable && !closing, std::memory_order_release);

    const bool retain = backgroundActive && !background.memoryLimited;
    if (command == stopActivity && keep && !retain) fallbackConnection.store(true, std::memory_order_release);
    CommandScope scope{command, retain, keep};
    if (appCommandOriginal) appCommandOriginal(app, command);

    if (command == startActivity) {
        background.memoryLimited = false;
        fallbackConnection.store(false, std::memory_order_release);
    }
    if (command == destroyActivity) background = {};
}

void trimMemoryDetour(void* activity, std::int32_t level) {
    if (level != trimUiHidden) memoryPressureRequested.store(true, std::memory_order_release);
    if (trimMemoryOriginal) trimMemoryOriginal(activity, level);
}

void shutdownDetour(void* env, void* activity) {
    shutdownRequested.store(true, std::memory_order_release);
    sessionActive.store(false, std::memory_order_release);
    throttleBackground.store(false, std::memory_order_release);
    fallbackConnection.store(false, std::memory_order_release);
    if (shutdownOriginal) shutdownOriginal(env, activity);
}

void clientUpdated(bedrocktools::events::ClientInstanceUpdateEvent& event) {
    const bool enabled = active();
    sessionActive.store(enabled && fastResumeReady.load(std::memory_order_acquire)
        && getLocalPlayer && event.clientInstance
        && getLocalPlayer(event.clientInstance) != nullptr, std::memory_order_release);

    if (!throttleBackground.load(std::memory_order_acquire)
        || shutdownRequested.load(std::memory_order_acquire)) {
        nextBackgroundTick = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (nextBackgroundTick > now) std::this_thread::sleep_until(nextBackgroundTick);
    nextBackgroundTick = std::chrono::steady_clock::now() + backgroundTickInterval;
}

template <class Function>
bedrocktools::hooks::Handle install(std::uintptr_t target, void* detour, Function** original) {
    return bedrocktools::hooks::install(reinterpret_cast<void*>(target), detour, reinterpret_cast<void**>(original));
}

void installFastResume() {
    const auto command = bedrocktools::memory::resolve(SignatureId::NoDisconnectAppCommand);
    const auto suspend = bedrocktools::memory::resolve(SignatureId::NoDisconnectAppSuspend);
    const auto resume = bedrocktools::memory::resolve(SignatureId::NoDisconnectAppResume);
    const auto trim = bedrocktools::memory::resolve(SignatureId::NoDisconnectTrimMemory);
    getLocalPlayer = reinterpret_cast<LocalPlayerFn>(bedrocktools::memory::resolve(SignatureId::ClientInstanceGetLocalPlayer));
    auto library = bedrocktools::hooks::openLibrary("libminecraftpe.so");
    const auto shutdown = bedrocktools::hooks::symbol(library, "Java_com_mojang_minecraftpe_MainActivity_nativeShutdown");
    bedrocktools::hooks::closeLibrary(library);
    if (!command || !suspend || !resume || !trim || !shutdown || !getLocalPlayer || !eduHook) return;

    fastHooks[0] = install(suspend, reinterpret_cast<void*>(suspendDetour), &suspendOriginal);
    if (fastHooks[0]) fastHooks[1] = install(resume, reinterpret_cast<void*>(resumeDetour), &resumeOriginal);
    if (fastHooks[1]) fastHooks[2] = install(trim, reinterpret_cast<void*>(trimMemoryDetour), &trimMemoryOriginal);
    if (fastHooks[2]) fastHooks[3] = install(shutdown, reinterpret_cast<void*>(shutdownDetour), &shutdownOriginal);
    if (fastHooks[3]) fastHooks[4] = install(command, reinterpret_cast<void*>(appCommandDetour), &appCommandOriginal);
    if (fastHooks[4]) {
        fastResumeReady.store(true, std::memory_order_release);
        return;
    }
    for (auto it = fastHooks.rbegin(); it != fastHooks.rend(); ++it) {
        bedrocktools::hooks::remove(*it);
        *it = nullptr;
    }
}
}

NoDisconnectModule::NoDisconnectModule()
    : Module("No Disconnect", "Avoids resource reloads when returning to menus or gameplay and keeps active sessions connected.") {}

void NoDisconnectModule::onInit() {
    if (initialized) return;
    initialized = true;
    const auto edu = bedrocktools::memory::resolve(SignatureId::EduMultiplayer);
    if (edu) eduHook = install(edu, reinterpret_cast<void*>(eduMultiplayerDetour), &eduMultiplayerOriginal);
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(clientUpdated);
    installFastResume();
}

void NoDisconnectModule::onEnable() {
    moduleActive.store(true, std::memory_order_release);
}

void NoDisconnectModule::onDisable() {
    moduleActive.store(false, std::memory_order_release);
    sessionActive.store(false, std::memory_order_release);
    fallbackConnection.store(false, std::memory_order_release);
}

void NoDisconnectModule::setRuntimeActive(bool state) {
    runtimeActive.store(state, std::memory_order_release);
    if (!state) {
        sessionActive.store(false, std::memory_order_release);
        fallbackConnection.store(false, std::memory_order_release);
    }
}

