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
    // are still accepted when loading older configs.
    std::uint32_t outlineColor = 0xFF00FFFFu;

    // Draws the targeted block as a translucent filled box (a true 3D volume)
    // in addition to the wireframe edges. Only the faces pointing at the
    // camera are drawn, so the tint stays on the block's surface instead of
    // bleeding through to the inside.
    bool outline3d = false;

    // Line size (menu slider units). 1.0 keeps the classic hairline box;
    // anything above that is drawn as real camera-facing geometry because GL
    // line width is ignored by most mobile GLES drivers. Only edges facing
    // the camera are drawn, so a thick outline still reads as a flat frame
    // rather than a see-through 3D wireframe.
    float lineThickness = 1.0f;

private:
    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};
