#pragma once

#include "../Module.hpp"
#include "coloredlights/ColoredLightsFeature.hpp"

class ColoredLightsModule : public Module {
public:
    ColoredLightsModule();
    ~ColoredLightsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void applySettings();

    bedrocktools::feature::coloredlights::Settings mSettings{};
    bool mInstalled = false;
};
