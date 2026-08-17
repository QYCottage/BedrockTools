#pragma once

#include "../Module.hpp"
#include <atomic>
#include <chrono>
#include <deque>
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
    bool onTouchEvent(float x, float y, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // True while the last input came from touch (mobile joystick / touch
    // buttons) instead of a mouse. Used to route GameMode events to the
    // touch button mapping.
    bool touchMode() const;

    // Mobile touch buttons have no mouse down/up events, so the LMB/RMB keys
    // are driven from the GameMode calls the buttons trigger:
    //   attack button -> onTouchAttack (lights the RMB key)
    //   build button  -> onTouchUse (lights the LMB key)
    void onTouchAttack();
    void onTouchAttackPulse();
    void onTouchUse();

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
    std::pair<int, int> getMouseCps();
    void clearMouseState();

    std::atomic_bool m_mouseActive{false};
    std::atomic_bool m_showMouseCps{true};
    std::atomic_bool m_lmbDown{false};
    std::atomic_bool m_rmbDown{false};
    std::deque<std::chrono::steady_clock::time_point> m_leftClicks;
    std::deque<std::chrono::steady_clock::time_point> m_rightClicks;
    std::mutex m_mouseMutex;

    // Last input activity timestamps (steady clock, microseconds) used to tell
    // touch input apart from mouse input.
    std::atomic<long long> m_lastTouchMs{0};
    std::atomic<long long> m_lastMouseMs{0};

    // Touch-button press pulses (steady clock, microseconds). The touch buttons have no
    // down/up events, so each GameMode call refreshes a short "pressed" pulse.
    // m_touchRmbMs = attack button presses (displayed on the RMB key),
    // m_touchLmbMs = build button presses (displayed on the LMB key).
    std::atomic<long long> m_touchRmbMs{0};
    std::atomic<long long> m_touchLmbMs{0};
};
