#pragma once

#include "../Module.hpp"

#include <bedrocktools/sdk/Types.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

class CinematicCameraModule : public Module {
public:
    CinematicCameraModule();
    ~CinematicCameraModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void applyTurnDelta(bedrocktools::sdk::Vec2& delta);

private:
    struct ConfigSnapshot {
        bool toggleMode;
        bool smoothing;
        float smoothness;
        bool cinebars;
        float cinebarHeight;
        std::uint32_t cinebarColor;
    };

    ConfigSnapshot snapshotConfig() const;
    void resetSmoothing();
    void clearBars();
    bool effectActive(const ConfigSnapshot& config) const;
    static std::uint32_t parseColor(const std::string& value, std::uint32_t fallback);

    mutable std::mutex m_configMutex;
    mutable std::mutex m_stateMutex;
    bedrocktools::sdk::Vec2 m_smoothDelta{0.0f, 0.0f};
    std::chrono::steady_clock::time_point m_lastTime = std::chrono::steady_clock::now();
    bool m_cameraActive = false;
    bool m_keyDown = false;

    bool m_toggleMode = false;
    bool m_smoothing = true;
    float m_smoothness = 8.0f;
    bool m_cinebars = false;
    std::string m_cinebarColor = "#FF000000";
    float m_cinebarHeight = 0.2f;
};
