#pragma once

#include "../Module.hpp"
#include <cstdint>

// Crosshair
//
// Replaces the vanilla crosshair (textures/gui/crosshair.png, drawn by
// HudCursorRenderer) with one of 16 custom shapes drawn through the HUD
// overlay. Color, scale, thickness, opacity, an outline pass and an
// animated RGB (rainbow) mode are configurable from the mod menu.
// Selecting the "Vanilla" style restores the game's own crosshair.png
// untouched - the module then neither hooks nor draws anything.
class CrosshairModule : public Module {
public:
    // The radio option order is persisted by index (see saveConfig), so new
    // styles must only ever be appended at the end, never inserted.
    enum class Style : int {
        Vanilla = 0,   // game default crosshair.png (module draws nothing)
        Cross,         // classic 4-arm crosshair with a center gap
        Dot,           // single center dot
        CrossDot,      // cross + center dot
        Circle,        // hollow circle
        CircleDot,     // hollow circle + center dot
        CircleCross,   // hollow circle with inner arms
        Square,        // hollow square
        SquareDot,     // hollow square + center dot
        Diamond,       // hollow diamond
        Plus,          // full plus sign, arms meet at the center
        X,             // diagonal cross
        TShape,        // top bar with a stem down to the center
        Chevron,       // upward pointing caret
        Arrow,         // upward pointing arrow
        Star,          // 8-spoke star/asterisk
        Scope,         // large circle with long thin arms (sniper style)
        Count
    };

    CrosshairModule();
    ~CrosshairModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // True while a custom shape is selected, i.e. the module replaces the
    // vanilla crosshair instead of letting the game draw crosshair.png.
    bool customStyleActive() const { return m_style != Style::Vanilla; }

    // Settings, mirrored into the mod menu (see saveConfig).
    Style m_style = Style::Cross;
    float m_scale = 1.0f;         // overall size multiplier (menu: 0.1 - 5)
    float m_thickness = 2.0f;     // line thickness in px (menu: 0 - 20)
    float m_opacity = 1.0f;       // 0 - 1
    // AARRGGBB; the alpha byte is rebuilt from m_opacity when drawing, so
    // the stored color only carries the RGB channels.
    uint32_t m_color = 0xFFFFFFFF;
    bool m_rgb = false;           // animated rainbow mode (overrides m_color)
    float m_rgbSpeed = 0.3f;      // full hue cycles per second (menu: 0.05 - 1)
    bool m_outline = true;        // dark back-pass so bright scenes stay readable

private:
    bool m_cursorHooked = false;  // HudCursorRenderer hook installed
};
