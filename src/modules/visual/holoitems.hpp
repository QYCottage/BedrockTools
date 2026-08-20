#pragma once

#include "../Module.hpp"

class HoloItemsModule : public Module {
public:
    HoloItemsModule();
    ~HoloItemsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // config
    int tickInterval = 5;      // update every N local-player ticks
    float radius = 30.0f;      // search radius in blocks
    bool showCount = true;     // append " x<count>" to the label
    bool colorEnabled = true;  // use a yellow (§e) color code
};
