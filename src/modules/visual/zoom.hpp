#pragma once

#include "../Module.hpp"

class ZoomModule : public Module {
public:
    float m_targetZoomFov = 20.58f;
    float m_currentFov = 90.0f;
    float m_baseFov = 90.0f;
    float m_animSpeed = 1.0f;
    float m_scrollSensitivity = 2.5f;
    bool  m_lowSens = true;
    float m_lowSensStrength = 0.9f;
    bool  m_hideHand = true;
    bool  m_overlayToggle = true;

    bool m_animationFinished = true;
    bool m_isFirstTime = true;

    bool m_keyZooming = false;
    bool m_buttonZooming = false;
    bool isZoomActive();

    ZoomModule();
    ~ZoomModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void adjustZoomByScroll(float delta);
    void updateZoomButton();

private:
    bool m_fovHooked = false;
    bool m_turnDeltaHooked = false;
    bool m_hideHandHooked = false;
};
