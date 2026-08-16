#pragma once
#include "../Module.hpp"
#include <cstdint>

class SwingModifierModule : public Module {
private:
    bool m_patched;
    uint8_t m_originalBytes[4];
    uint8_t m_originalBytes2[4];
    void* m_patchTarget;
    void* m_patchTarget2;
    bool m_getModifiedSwingDurationHooked;

    
    
    public:
    int m_swingSpeed = 30;
    bool m_fluxSwing = false;
    
    SwingModifierModule();
    ~SwingModifierModule() override;
    
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    void applyPatch();
    void removePatch();
};
