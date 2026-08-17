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
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    
    bool showEntities = true;
    bool showPlayers = true;
    bool showSelf = false;
    bool showEyeLine = false;
    bool showLookLine = false;
    float lookLineLength = 2.0f;

    // Line thickness (menu slider units). 1.0 keeps the classic hairline
    // look; anything above that is drawn as real geometry (beams around
    // every edge) whose world-space width is lineThickness * 0.01 blocks,
    // because GL line width is ignored by most mobile GL ES drivers.
    float lineThickness = 1.0f;

    
    // Stored as AARRGGBB. Alpha is always forced opaque when loading,
    // saving, and drawing so changing line thickness never washes the color out.
    uint32_t hitboxColor = 0xFFFFFFFF;
    uint32_t eyeLineColor = 0xFFFF0000;
    uint32_t lookLineColor = 0xFF0000FF; 

    bool hitboxIndicator = false;             
    uint32_t indicatorDefaultColor = 0xFFFFFFFF; 
    uint32_t indicatorActiveColor = 0xFFFF0000;  

    // Crosshair indicator: while the player is aiming at a mob or another
    // player that is close enough to actually be hit (the game's own hit
    // result decides the reach, not a configurable range), the vanilla
    // crosshair itself is recolored in place to crosshairIndicatorColor via
    // the HudCursorRenderer + Tessellator::color hooks - no second
    // crosshair is drawn over it. If the running game build makes in-place
    // tinting impossible, the vanilla crosshair is hidden while the
    // indicator is active and a matching crosshair is drawn instead, so
    // there is never a stacked crosshair either way.
    bool crosshairIndicator = false;
    uint32_t crosshairIndicatorColor = 0xFFFF0000;

private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    // Crosshair-indicator hooks (installed once in onInit, chainable with
    // the debug menu's HudCursor hook).
    bool m_cursorHooked;
    bool m_tessColorHooked;

    void applyPatch();
};
