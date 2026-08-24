#pragma once

#include "../Module.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdint>
#include <mutex>
#include <utility>

class KeystrokesModule : public Module {
public:
    KeystrokesModule();
    ~KeystrokesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onMouseEvent(int button, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool bW = false;
    bool bA = false;
    bool bS = false;
    bool bD = false;
    bool bSpace = false;
    bool bSneak = false;

    int m_size = 50;
    bool m_showJump = true;
    bool m_showSneak = true;
    bool m_roundKeys = true;
    uint32_t m_pressedColor = 0xFF00FF00;
    bool m_rainbow = false;
    float m_rainbowSpeed = 1.0f;
    float m_rainbowHue = 0.0f;

    struct KeyAnimState {
        float pressProgress = 0.0f;
    };

    KeyAnimState m_wState;
    KeyAnimState m_aState;
    KeyAnimState m_sState;
    KeyAnimState m_dState;
    KeyAnimState m_jumpState;
    KeyAnimState m_sneakState;
    KeyAnimState m_lmbState;
    KeyAnimState m_rmbState;

    float hudPosX = 20.0f;
    float hudPosY = 100.0f;
    bool isHudModule = true;

private:
    using PlayerSwingFn = bool(*)(void*, std::uint8_t);
    static bool playerSwingDetour(void* player, std::uint8_t source);
    static PlayerSwingFn s_playerSwingOriginal;

    std::pair<int, int> getMouseCps();
    void clearMouseState();
    void queueNativeClick(bool left);
    void queueNativeSwing(std::uint8_t source);
    void queueNativeExplicitLeft();
    void queueNativeRight(std::uint16_t swingSourceMask);
    void resolveNativeInputTick();
    void flushNativeClickBatch(bool force);
    void commitNativeClickBatchLocked();

    bool m_playerSwingHooked = false;
    std::atomic_bool m_destroyActive{false};
    std::atomic_bool m_mouseActive{false};
    std::atomic_bool m_showMouseCps{true};
    std::atomic_bool m_lmbDown{false};
    std::atomic_bool m_rmbDown{false};
    std::atomic<std::int64_t> m_nativeLmbUntilNs{0};
    std::atomic<std::int64_t> m_nativeRmbUntilNs{0};
    std::atomic<std::int64_t> m_lastMouseLmbNs{0};
    std::atomic<std::int64_t> m_lastMouseRmbNs{0};
    std::deque<std::chrono::steady_clock::time_point> m_leftClicks;
    std::deque<std::chrono::steady_clock::time_point> m_rightClicks;
    bool m_nativeBatchActive = false;
    bool m_nativeBatchLeft = false;
    bool m_nativeBatchRight = false;
    std::int64_t m_nativeBatchLastNs = 0;
    struct PendingNativeSwing {
        std::uint8_t source = 0;
        std::uint8_t ticksRemaining = 0;
    };
    std::deque<PendingNativeSwing> m_pendingNativeSwings;
    std::uint32_t m_nativeSwingSuppressCount = 0;
    std::uint8_t m_nativeSwingSuppressTicks = 0;
    std::uint16_t m_nativeRmbSwingSuppressMask = 0;
    std::uint8_t m_nativeRmbSwingSuppressTicks = 0;
    bool m_nativeExplicitLeftThisTick = false;
    bool m_nativeRightThisTick = false;
    std::mutex m_mouseMutex;
};
