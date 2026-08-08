#pragma once

// Tablist module — thanks to Kashifro
// GitHub: https://github.com/Kashifro

#include "../Module.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct TablistPlayerInfo {
    std::string name;
    std::string imageId;
};

class TablistModule : public Module {
public:
    TablistModule();
    ~TablistModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    void onLocalPlayerTick(void* localPlayer);

    float hudPosX = 100.f;
    float hudPosY = 50.f;
    bool isHudModule = false;

    float m_textSize = 30.f;
    float m_colWidth = 250.f;
    int m_maxColumns = 2;
    uint32_t m_textColorHex = 0xFFFFFFFF;
    uint32_t m_bgColorHex = 0x80000000;
    bool m_showHeads = true;

    static TablistModule* getInstance();

private:
    std::mutex m_mutex;
    std::vector<TablistPlayerInfo> m_players;
    int m_refreshTicks = 20;
    static TablistModule* s_instance;
};
