#pragma once

#include "../Module.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bedrocktools::sdk { class Player; }

class EffectDisplayModule final : public Module {
public:
    struct ActiveEffect {
        std::uint32_t id = 0;
        int durationTicks = 0;
        int amplifier = 0;
    };

    EffectDisplayModule();
    ~EffectDisplayModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void updateEffects(bedrocktools::sdk::Player* player);

private:
    void registerResources();

    std::mutex m_mutex;
    std::vector<ActiveEffect> m_effects;
    bool m_resourcesRegistered = false;

    float hudPosX = 8.0f;
    float hudPosY = 70.0f;
    bool isHudModule = true;

    float m_scale = 1.0f;
    float m_width = 210.0f;
    float m_backgroundOpacity = 0.82f;
    bool m_showBackground = true;
    bool m_showIcons = true;
    bool m_showLevel = true;
    bool m_preview = false;
    int m_maxVisible = 36;
};
