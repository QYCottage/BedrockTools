#pragma once

#include "../Module.hpp"

class BushyLeavesModule final : public Module {
public:
    BushyLeavesModule();
    ~BushyLeavesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int layers = 3;
    float spread = 0.08f;
    bool affectTopBottom = true;
    bool affectSides = true;

private:
    bool m_hooked = false;
    void installHooks();
    void applySettings();
};
