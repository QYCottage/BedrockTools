#pragma once

#include "../Module.hpp"

class AutoGG : public Module {
public:
    static AutoGG* instance;
    
    AutoGG();
    ~AutoGG() override;
    
    void onInit() override;
    void onDisable() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    
    std::string ggMessage = "gg";
    int delaySeconds = 0;
};
