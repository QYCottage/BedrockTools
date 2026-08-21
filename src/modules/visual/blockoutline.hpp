#pragma once

#include "../Module.hpp"
#include <cstdint>

class BlockOutlineModule final : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    // Stored as AARRGGBB with alpha forced opaque. The menu exposes this as a
    // single RGB color picker (saved as "#RRGGBB", like the Hitbox colors)
    // instead of the old Red/Green/Blue sliders. The legacy per-channel keys
    // are still accepted when loading older configs. Only used while `rgb`
    // (rainbow mode) is off.
    std::uint32_t outlineColor = 0xFF00FFFFu;

    // Opt-in menu toggle ("Block 3d"): draws the targeted block as a
    // translucent filled box (a true 3D volume) in addition to the wireframe
    // edges. Defaults to off, so the plain wireframe is what players get
    // unless they explicitly enable the 3D box. Only the faces pointing at
    // the camera are drawn, so the tint stays on the block's surface instead
    // of bleeding through to the inside.
    bool block3d = false;

    // Rainbow mode ("Rgb" menu toggle): while enabled, the outline color
    // cycles continuously through the RGB spectrum and overrides the static
    // `outlineColor` picker. The cycling color is applied to every pass
    // (hairline, thick edges and the 3D fill).
    bool rgb = false;

    // Line size (menu slider units). 1.0 keeps the classic hairline box;
    // anything above that is drawn as real camera-facing geometry because GL
    // line width is ignored by most mobile GLES drivers. In thick mode only
    // the block's projected outline (silhouette edges) is drawn, so raising
    // the size thickens a flat frame instead of turning it into a 3D box —
    // that is what the explicit "Block 3d" toggle is for.
    float lineThickness = 1.0f;

private:
    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};
