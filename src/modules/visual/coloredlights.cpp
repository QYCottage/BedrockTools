#include "coloredlights.hpp"

#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

float parseFloat(std::string_view value, float fallback) {
    std::string text(value);
    char* end = nullptr;
    const float result = std::strtof(text.c_str(), &end);
    return end && end != text.c_str() ? result : fallback;
}

bool parseBool(std::string_view value, bool fallback) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

}

ColoredLightsModule::ColoredLightsModule()
    : Module("Colored Lights", "Colored block lighting without Vibrant Visuals.") {
    hideInHudEditor = true;
}

ColoredLightsModule::~ColoredLightsModule() {
    if (mInstalled) bedrocktools::feature::coloredlights::shutdown();
}

void ColoredLightsModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [this](auto& event) {
            if (enabled) {
                bedrocktools::feature::coloredlights::onClientInstanceUpdate(event.clientInstance);
            }
        });
}

void ColoredLightsModule::onEnable() {
    if (!mInstalled) {
        mInstalled = bedrocktools::feature::coloredlights::install(mSettings, true);
        return;
    }
    bedrocktools::feature::coloredlights::updateSettings(mSettings);
    bedrocktools::feature::coloredlights::setActive(true);
}

void ColoredLightsModule::onDisable() {
    if (mInstalled) bedrocktools::feature::coloredlights::setActive(false);
}

void ColoredLightsModule::applySettings() {
    mSettings.colorIntensity = std::clamp(mSettings.colorIntensity, 0.0f, 1.0f);
    mSettings.lightSpread = std::clamp(mSettings.lightSpread, 0.5f, 1.5f);
    if (mInstalled) bedrocktools::feature::coloredlights::updateSettings(mSettings);
}

bool ColoredLightsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "colorIntensity") mSettings.colorIntensity = parseFloat(value, mSettings.colorIntensity);
    else if (key == "lightSpread") mSettings.lightSpread = parseFloat(value, mSettings.lightSpread);
    else if (key == "lightThroughWalls") mSettings.lightThroughWalls = parseBool(value, mSettings.lightThroughWalls);
    else if (key == "customizeColors") mSettings.customizeColors = parseBool(value, mSettings.customizeColors);
    else if (key == "redstoneColor") mSettings.redstoneColor = std::string(value);
    else if (key == "soulColor") mSettings.soulColor = std::string(value);
    else if (key == "seaColor") mSettings.seaColor = std::string(value);
    else if (key == "warmColor") mSettings.warmColor = std::string(value);
    else if (key == "purpleColor") mSettings.purpleColor = std::string(value);
    else if (key == "greenColor") mSettings.greenColor = std::string(value);
    else return false;
    applySettings();
    return true;
}

void ColoredLightsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("colorIntensity")) mSettings.colorIntensity = j["colorIntensity"].get<float>();
    if (j.contains("lightSpread")) mSettings.lightSpread = j["lightSpread"].get<float>();
    if (j.contains("lightThroughWalls")) mSettings.lightThroughWalls = j["lightThroughWalls"].get<bool>();
    if (j.contains("customizeColors")) mSettings.customizeColors = j["customizeColors"].get<bool>();
    if (j.contains("redstoneColor")) mSettings.redstoneColor = j["redstoneColor"].get<std::string>();
    if (j.contains("soulColor")) mSettings.soulColor = j["soulColor"].get<std::string>();
    if (j.contains("seaColor")) mSettings.seaColor = j["seaColor"].get<std::string>();
    if (j.contains("warmColor")) mSettings.warmColor = j["warmColor"].get<std::string>();
    if (j.contains("purpleColor")) mSettings.purpleColor = j["purpleColor"].get<std::string>();
    if (j.contains("greenColor")) mSettings.greenColor = j["greenColor"].get<std::string>();
    applySettings();
}

void ColoredLightsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["colorIntensity"] = mSettings.colorIntensity;
    j["lightSpread"] = mSettings.lightSpread;
    j["lightThroughWalls"] = mSettings.lightThroughWalls;
    j["customizeColors"] = mSettings.customizeColors;
    j["redstoneColor"] = mSettings.redstoneColor;
    j["soulColor"] = mSettings.soulColor;
    j["seaColor"] = mSettings.seaColor;
    j["warmColor"] = mSettings.warmColor;
    j["purpleColor"] = mSettings.purpleColor;
    j["greenColor"] = mSettings.greenColor;
}
