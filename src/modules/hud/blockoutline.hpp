#pragma once

#include "../Module.hpp"
#include <cstdint>

class BlockOutlineModule : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // ARGB: 0xAARRGGBB
    uint32_t outlineColor = 0xFFFFFFFF;

    // Cycle through the RGB spectrum instead of using outlineColor's RGB channels.
    bool rgb = false;
    float rgbSpeed = 1.0f;
    float rgbHue = 0.0f;

    // When true, the outline is drawn as a real 3D frame (volumetric edge
    // beams sitting outside the block) and rendered through walls so every
    // edge stays visible, including the ones behind the block.
    bool threeD = false;

    // Distance from the block surface. Applied as a small outset so the
    // outline is not buried inside the block (and hidden by depth testing).
    float inset = 0.0025f;

    // Line thickness (menu slider units). The world-space band / beam width
    // is thickness * 0.01 blocks.
    float thickness = 2.0f;

    // Maximum distance from the camera at which the outline is drawn.
    float maxDistance = 128.0f;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};
