#pragma once
#include "../Module.hpp"
#include <cstdint>

namespace bedrocktools::sdk {
class ClientInstance;
class Player;
}

class ThirdPersonNametagModule : public Module {
private:
    bool m_patched;
    uint8_t m_originalBytes[4];
    void* m_patchTarget;

    void applyPatch();
    void removePatch();

    // --- Nametag Icon ------------------------------------------------------
    // Draws the launcher icon (BedrockTools logo) above your own head while
    // the camera is in third person, complementing the nametag this module
    // restores. The icon is a screen-space overlay anchored to the head, so
    // it stays readable at any distance (same idea as a vanilla nametag).
    void registerResources();

    bool m_resourcesRegistered = false;

    // Resolved once in onInit so onFrame stays cheap (no per-frame dlopen).
    using GetLocalPlayerFn = bedrocktools::sdk::Player* (*)(bedrocktools::sdk::ClientInstance*);
    GetLocalPlayerFn m_getLocalPlayer = nullptr;

    // "Nametag Icon" toggle and its on-screen size multiplier.
    bool m_nametagIcon = true;
    float m_iconScale = 1.0f;

public:
    ThirdPersonNametagModule();
    ~ThirdPersonNametagModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
};
