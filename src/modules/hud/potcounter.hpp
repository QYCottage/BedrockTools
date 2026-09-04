#pragma once

#include "../Module.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace bedrocktools::sdk { class Player; }

class PotCounterModule : public Module {
public:
    PotCounterModule();
    void onInit() override;
    void onDisable() override;
    void onMenuRegistered() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void updateCount(bedrocktools::sdk::Player* player);
    std::atomic<int> m_count{0};
    std::mutex m_configMutex;
    float hudPosX = 20.0f;
    float hudPosY = 460.0f;
    bool isHudModule = true;
    float m_size = 40.0f;
    bool m_background = true;
    std::string m_backgroundColor = "#000000";
    float m_backgroundOpacity = 0.5f;
    std::string m_textColor = "#FFFFFF";
    bool m_showZero = true;
};
