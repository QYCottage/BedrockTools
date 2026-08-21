#pragma once

#include "../Module.hpp"
#include <string>
#include <unordered_map>
#include <cstdint>
#include <mutex>

class MobHealthIndicatorModule : public Module {
public:
    MobHealthIndicatorModule();
    ~MobHealthIndicatorModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Settings
    int m_displayMode = 0;       // 0 = hearts, 1 = bar
    float m_maxDistance = 20.0f; // max distance to show health
    bool m_showMobs = true;      // show health for mobs only
    int m_maxHearts = 10;        // number of hearts to display (for hearts mode)
    float m_barLength = 10.0f;   // number of segments in the bar (for bar mode)

private:
    void onLocalPlayerTick(void* localPlayer);
    void restoreAllNametags();
    std::string formatHealthHearts(float health, float maxHealth);
    std::string formatHealthBar(float health, float maxHealth);

    int m_refreshTicks = 20;
    void* m_localPlayer = nullptr;

    struct OriginalNametagState {
        std::string name;
        bool hadAlwaysShow = false;
        int8_t alwaysShowValue = 0;
    };

    std::mutex m_mutex;
    std::unordered_map<void*, OriginalNametagState> m_originalStates;
    std::unordered_map<void*, float> m_lastHealth; // track last displayed health to avoid redundant updates
};
