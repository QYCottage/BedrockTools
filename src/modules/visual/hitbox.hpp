#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>

class HitboxModule : public Module {
public:
    HitboxModule();
    ~HitboxModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    
    bool showEntities = true;
    bool showPlayers = true;
    bool showSelf = true;
    bool showEyeLine = true;
    bool showLookLine = true;
    float lookLineLength = 2.0f;

    
    uint32_t hitboxColor = 0xFFFFFFFF;   
    uint32_t eyeLineColor = 0xFFFF0000;  
    uint32_t lookLineColor = 0xFF0000FF; 

    bool hitboxIndicator = false;             
    uint32_t indicatorDefaultColor = 0xFFFFFFFF; 
    uint32_t indicatorActiveColor = 0xFFFF0000;  
    float hitRange = 3.0f;                         

private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    void applyPatch();
};
