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

    // Stored as AARRGGBB. The menu exposes the RGB channels as three sliders
    // (outlineRed/Green/Blue) and keeps alpha forced opaque when drawing, so
    // the color never washes out. The hex picker key is kept for loading
    // older configs but is no longer written.
    std::uint32_t outlineColor = 0xFF00FFFFu;

    // Draws the targeted block as a translucent filled box (a true 3D volume)
    // in addition to the wireframe edges.
    bool outline3d = false;

    // Line size (menu slider units). 1.0 keeps the classic hairline box;
    // anything above that is drawn as real camera-facing geometry because GL
    // line width is ignored by most mobile GLES drivers.
    float lineThickness = 1.0f;

    // RGB channels (0-255) mirrored to/from outlineColor.
    int outlineRed = 0;
    int outlineGreen = 255;
    int outlineBlue = 255;

private:
    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};
