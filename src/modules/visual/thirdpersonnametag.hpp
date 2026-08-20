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
    // Draws the launcher icon (the BedrockTools logo shipped as icon.png in
    // the levipack) directly to the LEFT of your own name while the camera is
    // in third person, like a rank badge in front of the nametag this module
    // restores. The icon is a screen-space overlay whose position and size are
    // derived from the nametag's own projection, so it tracks the name at any
    // distance and camera angle.
    void registerResources();

    // Visible character count of the name the game paints above the head,
    // used to find the left edge of the nametag.
    int localNameLength(bedrocktools::sdk::Player* player) const;

    bool m_resourcesRegistered = false;

    // Resolved once in onInit so onFrame stays cheap (no per-frame dlopen).
    using GetLocalPlayerFn = bedrocktools::sdk::Player* (*)(bedrocktools::sdk::ClientInstance*);
    GetLocalPlayerFn m_getLocalPlayer = nullptr;

    // "Nametag Icon" toggle and its size multiplier (1.0 == the icon is a
    // little taller than the name text).
    bool m_nametagIcon = true;
    float m_nametagIconScale = 1.0f;

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
