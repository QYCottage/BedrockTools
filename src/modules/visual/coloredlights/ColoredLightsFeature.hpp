#pragma once

#include <string>

namespace bedrocktools::feature::coloredlights {

struct Settings {
    float colorIntensity = 0.85f;
    float lightSpread = 1.0f;
    bool lightThroughWalls = true;
    bool customizeColors = false;
    std::string redstoneColor = "#FF321FFF";
    std::string soulColor = "#438DFFFF";
    std::string seaColor = "#59D7FFFF";
    std::string warmColor = "#FFB45CFF";
    std::string purpleColor = "#B970FFFF";
    std::string greenColor = "#72F29AFF";
};

bool install(const Settings& settings, bool active);
void setActive(bool active);
void updateSettings(const Settings& settings);
void onClientInstanceUpdate(void* clientInstance);
bool setConnectedGlassActive(bool active);
void shutdown();

}
