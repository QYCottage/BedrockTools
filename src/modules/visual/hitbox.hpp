#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>

// Queried by the shared HUD cursor hook. Keeping the query here makes the
// indicator belong to the Hitbox module without installing a second hook on
// HudCursor (Debug Menu already owns that hook).
//
// While the Crosshair Indicator is active the vanilla (white) cursor draw is
// skipped entirely and the Hitbox module draws its own red crosshair in the
// centre of the screen from onFrame(). As soon as the target leaves melee
// reach the original cursor comes back untouched.
bool shouldHideVanillaCrosshair();

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
    bool showSelf = true;
    bool showEyeLine = true;
    bool showLookLine = true;

    // Line thickness (menu slider units). Applied only to the hitbox outline
    // (the white box) — eye / look lines stay 1px. World-space width is
    // thickness * 0.01 blocks. 0 keeps the classic 1px GL_LINES look.
    float thickness = 1.0f;

    uint32_t hitboxColor = 0xFFFFFFFF;
    uint32_t eyeLineColor = 0xFFFF0000;

    // Hitbox Indicator: while enabled, an entity's hitbox uses
    // hitboxIndicatorColor (red by default) whenever the local player can
    // actually melee it — i.e. the game's pick ray (or the aimed entity from
    // HitResult) reaches the hitbox within 3 blocks. Standing next to / above
    // / below an entity without looking at it does not trigger the color. It
    // returns to the normal color as soon as the entity leaves that aimed
    // reach window.
    bool hitboxIndicator = true;
    float hitboxIndicatorRange = 3.0f;
    uint32_t hitboxIndicatorColor = 0xFFFF0000;

    // Crosshair Indicator: independent of both the box recolor above and the
    // hitbox Players/Entities toggles. When on and a mob / player is aimed at
    // within melee reach, the vanilla white cursor is hidden and a red
    // crosshair is drawn at the centre of the screen. The original cursor is
    // restored the moment the target leaves reach.
    bool crosshairIndicator = true;
    uint32_t crosshairIndicatorColor = 0xFFFF0000;
    float crosshairIndicatorSize = 9.0f;
    float crosshairIndicatorThickness = 2.0f;

private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    void applyPatch();
};
