#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <atomic>

class AimAssistModule final : public Module {
public:
    AimAssistModule();
    ~AimAssistModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void processPlayerTick(bedrocktools::sdk::Player* player);
    void applyTurnDelta(bedrocktools::sdk::Vec2& delta);

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    bool m_weaponOnly = false;
    float m_range = 5.5f;
    float m_fov = 35.0f;
    float m_strength = 0.12f;

private:
    void clearTargetState();
    void resetCameraState();
    std::atomic<float> m_cameraPitch{0.0f};
    std::atomic<float> m_cameraYaw{0.0f};
    std::atomic<float> m_pendingYawError{0.0f};
    std::atomic<float> m_pendingPitchError{0.0f};
    std::atomic<float> m_pendingFov{35.0f};
    std::atomic<float> m_pendingStrength{0.0f};
    std::atomic<bool> m_cameraRotationInitialized{false};
    void* m_lastTarget = nullptr;
};

void applyAimAssistTurnDelta(bedrocktools::sdk::Vec2& delta);
