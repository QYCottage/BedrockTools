// blockoutline_core.hpp
//
// Platform-agnostic core for the Block Outline module.
//
// This header owns every piece of the module that does not require
// the live Minecraft Bedrock process in memory: the geometry builders
// (12 edge lines + thick face-band quads + 12 volumetric 3D beam quads),
// the HSV → RGB cycle used by the rainbow outline mode, and the bounds
// check used to reject corrupt HitResult snapshots before they reach
// the renderer.
//
// The native cpp (src/modules/visual/blockoutline.cpp) installs the
// ARM64 hook, resolves tessellator / render-mesh / material-group
// pointers from the live game binary, subscribes to LocalPlayerTickEvent
// to refresh the HitSnapshot, and then calls into this core to assemble
// the outline geometry around the selected block. The host-side test
// driver (host_test/blockoutline_test.cpp) links this same core file
// directly with stock g++ -std=c++20 so the math is exercised end-to-end
// on Linux without needing the game, the Android NDK, or the Preloader.

#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <cstdint>
#include <vector>

namespace bedrocktools::blockoutline {

// Two endpoints in world space. addBoxLines pushes 12 of these (the
// twelve edges of an AABB).
struct Line {
    sdk::Vec3 a;
    sdk::Vec3 b;
};

// One flat quad in world space. addThickOutlineQuads pushes 24 (six
// faces * four rectangular pieces that share the band's thickness),
// and addBoxBeamQuads pushes 72 (twelve beams * six cuboid faces).
struct Quad {
    sdk::Vec3 v[4];
};

// Snapshot of the player's current HitResult, captured on the game
// thread inside the LocalPlayerTickEvent callback. Stored off-thread
// because the render hook runs on the render thread and must not call
// back into game code.
struct BlockHitSnapshot {
    sdk::BlockPos blockPos{};
    sdk::Vec3 hitPos{};
    bool valid = false;
};

// Tunable knobs actually used by the geometry builder. The module
// class clamps them before buildOutlineGeometry() is called, so the
// core itself stays free of policy.
struct GeometryOptions {
    // Outward inset applied to the AABB so the outline sits outside
    // the block and depth testing does not bury it inside the solid.
    float gap = 0.0025f;

    // Thickness slider value (the same units the menu uses). The
    // builder converts it into the band/beam width internally.
    float thickness = 2.0f;

    // When true the outline is drawn as 12 volumetric beams (3D frame).
    bool threeD = false;
};

// Convenience bundle so the host test driver and the native module
// stay in sync on what colours mean. ARGB = 0xAARRGGBB, matching
// how the cpp emits ARGB into the colour-holder float[4].
struct Rgb {
    float r = 0.0f; // ((c >> 16) & 0xFF) / 255
    float g = 0.0f; // ((c >>  8) & 0xFF) / 255
    float b = 0.0f; // ((c      ) & 0xFF) / 255
    float a = 0.0f; // ((c >> 24) & 0xFF) / 255
};

// ── Geometry builders ─────────────────────────────────────────────────

// Pushes 12 lines that trace every edge of the [min,max] AABB into
// outLines. Used for both the 2D crisper pass and the outer skeleton
// of the 3D frame.
void addBoxLines(std::vector<Line>& outLines,
                 const sdk::Vec3& min,
                 const sdk::Vec3& max);

// Pushes 24 quads (six face bands, each comprising four rectangular
// pieces that together cover the perimeter exactly once) to outline
// the box with the 2D "thick bands" pass. `thickness` is in blocks
// (already slider-converted, so it is normally in [0, 0.45]). The
// caller is expected to skip the call when thickness is near zero.
void addThickOutlineQuads(std::vector<Quad>& outQuads,
                          const sdk::Vec3& min,
                          const sdk::Vec3& max,
                          float thickness);

// Pushes 72 quads (twelve axis-aligned rectangular beams, six faces
// each) that wrap the edges of the box as volumetric tubes, so the
// 3D mode keeps the far edges visible through the block. `halfWidth`
// is the half-thickness of every beam; the geometry extends each
// beam by `halfWidth` past the corner on both ends so the eight
// corners stay solid.
void addBoxBeamQuads(std::vector<Quad>& outQuads,
                    const sdk::Vec3& min,
                    const sdk::Vec3& max,
                    float halfWidth);

// Builds the full set of lines + quads for the outline, applying the
// module's threeD toggle to pick the 2D-bands or 3D-beams path. The
// returned vectors are safe to use directly with the in-game
// tessellator or with the host-side ASCII/SVG preview.
void buildOutlineGeometry(const sdk::BlockPos& block,
                          const GeometryOptions& opt,
                          std::vector<Line>& outLines,
                          std::vector<Quad>& outQuads);

// ── Snapshot validation ────────────────────────────────────────────────

// Rejects snapshots whose block / hit coordinates are corrupt before
// they reach the renderer. In particular stops NaN/Inf positions and
// hit coordinates that obviously do not live on the selected block —
// otherwise the renderer produces astronomical vertex translations.
bool validHitSnapshot(const BlockHitSnapshot& snapshot);

// ── Colour helpers ─────────────────────────────────────────────────────

// Standard HSV → RGB conversion (S = saturation, V = value, both in
// 0..1). `h` is taken modulo 1.0 with negative values wrapped.
void hsvToRgb(float h, float s, float v,
              float& outR, float& outG, float& outB);

// Unpacks the ARGB value carried in BlockOutlineModule::outlineColor
// into the [0,1] float channels the renderer needs to push into
// Tessellator::color().
Rgb unpackArgb(uint32_t argb);

// Advances the hue state used by the rainbow outline, wraps it into
// [0, 1), and rebuilds the RGB channels of `baseArgb` (the alpha
// channel is preserved). When `rgbEnabled` is false, returns
// `baseArgb` unchanged and `hueState` is left alone.
uint32_t cycleRgbColor(uint32_t baseArgb,
                       bool rgbEnabled,
                       float& hueState,
                       float rgbSpeed,
                       float tick = 0.0f);

} // namespace bedrocktools::blockoutline
