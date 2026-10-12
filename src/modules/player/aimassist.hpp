#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>

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

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    float m_range = 5.5f;
    float m_fov = 35.0f;
    float m_strength = 0.12f;

private:
    void* m_lastTarget = nullptr;
};
