// blockoutline_core.cpp
//
// See blockoutline_core.hpp for the design contract. Every function
// here is platform-agnostic so the same translation unit compiles
// on Linux for the host-side test driver and on Android for the
// in-game renderer.

#include "blockoutline_core.hpp"

#include <algorithm>
#include <cmath>

namespace bedrocktools::blockoutline {

void addBoxLines(std::vector<Line>& outLines,
                 const sdk::Vec3& min,
                 const sdk::Vec3& max) {
    // Bottom square.
    outLines.push_back({{min.x, min.y, min.z}, {max.x, min.y, min.z}});
    outLines.push_back({{max.x, min.y, min.z}, {max.x, min.y, max.z}});
    outLines.push_back({{max.x, min.y, max.z}, {min.x, min.y, max.z}});
    outLines.push_back({{min.x, min.y, max.z}, {min.x, min.y, min.z}});

    // Top square.
    outLines.push_back({{min.x, max.y, min.z}, {max.x, max.y, min.z}});
    outLines.push_back({{max.x, max.y, min.z}, {max.x, max.y, max.z}});
    outLines.push_back({{max.x, max.y, max.z}, {min.x, max.y, max.z}});
    outLines.push_back({{min.x, max.y, max.z}, {min.x, max.y, min.z}});

    // Vertical edges.
    outLines.push_back({{min.x, min.y, min.z}, {min.x, max.y, min.z}});
    outLines.push_back({{max.x, min.y, min.z}, {max.x, max.y, min.z}});
    outLines.push_back({{max.x, min.y, max.z}, {max.x, max.y, max.z}});
    outLines.push_back({{min.x, min.y, max.z}, {min.x, max.y, max.z}});
}

void addThickOutlineQuads(std::vector<Quad>& outQuads,
                          const sdk::Vec3& min,
                          const sdk::Vec3& max,
                          float thickness) {
    const float x0 = min.x, y0 = min.y, z0 = min.z;
    const float x1 = max.x, y1 = max.y, z1 = max.z;
    const float t = thickness;

    // Rectangle on a horizontal (Y-facing) plane.
    const auto rectY = [&](float y, float ax0, float ax1,
                            float az0, float az1) {
        outQuads.push_back({{
            {ax0, y, az0}, {ax1, y, az0}, {ax1, y, az1}, {ax0, y, az1}
        }});
    };
    const auto rectZ = [&](float z, float ax0, float ax1,
                            float ay0, float ay1) {
        outQuads.push_back({{
            {ax0, ay0, z}, {ax1, ay0, z}, {ax1, ay1, z}, {ax0, ay1, z}
        }});
    };
    const auto rectX = [&](float x, float ay0, float ay1,
                            float az0, float az1) {
        outQuads.push_back({{
            {x, ay0, az0}, {x, ay1, az0}, {x, ay1, az1}, {x, ay0, az1}
        }});
    };

    // Top and bottom: two full-length bands plus two shorter bands so
    // the four corners are covered exactly once.
    for (float y : {y0, y1}) {
        rectY(y, x0, x1, z0, z0 + t);
        rectY(y, x0, x1, z1 - t, z1);
        rectY(y, x0, x0 + t, z0 + t, z1 - t);
        rectY(y, x1 - t, x1, z0 + t, z1 - t);
    }
    // North/south.
    for (float z : {z0, z1}) {
        rectZ(z, x0, x1, y0, y0 + t);
        rectZ(z, x0, x1, y1 - t, y1);
        rectZ(z, x0, x0 + t, y0 + t, y1 - t);
        rectZ(z, x1 - t, x1, y0 + t, y1 - t);
    }
    // East/west.
    for (float x : {x0, x1}) {
        rectX(x, y0, y0 + t, z0, z1);
        rectX(x, y1 - t, y1, z0, z1);
        rectX(x, y0 + t, y1 - t, z0, z0 + t);
        rectX(x, y0 + t, y1 - t, z1 - t, z1);
    }
}

void addBoxBeamQuads(std::vector<Quad>& outQuads,
                    const sdk::Vec3& min,
                    const sdk::Vec3& max,
                    float halfWidth) {
    const float x0 = min.x, y0 = min.y, z0 = min.z;
    const float x1 = max.x, y1 = max.y, z1 = max.z;

    // Six faces of an axis-aligned cuboid, written in both windings
    // so the band is visible from either side once quads are emitted.
    const auto cuboid = [&](float ax0, float ay0, float az0,
                            float ax1, float ay1, float az1) {
        outQuads.push_back({{ {ax0, ay0, az0}, {ax0, ay1, az0}, {ax0, ay1, az1}, {ax0, ay0, az1} }});
        outQuads.push_back({{ {ax1, ay0, az0}, {ax1, ay1, az0}, {ax1, ay1, az1}, {ax1, ay0, az1} }});
        outQuads.push_back({{ {ax0, ay0, az0}, {ax1, ay0, az0}, {ax1, ay0, az1}, {ax0, ay0, az1} }});
        outQuads.push_back({{ {ax0, ay1, az0}, {ax1, ay1, az0}, {ax1, ay1, az1}, {ax0, ay1, az1} }});
        outQuads.push_back({{ {ax0, ay0, az0}, {ax1, ay0, az0}, {ax1, ay1, az0}, {ax0, ay1, az0} }});
        outQuads.push_back({{ {ax0, ay0, az1}, {ax1, ay0, az1}, {ax1, ay1, az1}, {ax0, ay1, az1} }});
    };

    // Beams parallel to X, one at every (y, z) corner pair, extending
    // outward by halfWidth on either side of the box.
    for (float y : {y0, y1}) {
        const float ya = (y == y0) ? y - halfWidth : y;
        const float yb = (y == y0) ? y             : y + halfWidth;
        for (float z : {z0, z1}) {
            const float za = (z == z0) ? z - halfWidth : z;
            const float zb = (z == z0) ? z             : z + halfWidth;
            cuboid(x0 - halfWidth, ya, za, x1 + halfWidth, yb, zb);
        }
    }
    // Beams parallel to Y.
    for (float x : {x0, x1}) {
        const float xa = (x == x0) ? x - halfWidth : x;
        const float xb = (x == x0) ? x             : x + halfWidth;
        for (float z : {z0, z1}) {
            const float za = (z == z0) ? z - halfWidth : z;
            const float zb = (z == z0) ? z             : z + halfWidth;
            cuboid(xa, y0 - halfWidth, za, xb, y1 + halfWidth, zb);
        }
    }
    // Beams parallel to Z.
    for (float x : {x0, x1}) {
        const float xa = (x == x0) ? x - halfWidth : x;
        const float xb = (x == x0) ? x             : x + halfWidth;
        for (float y : {y0, y1}) {
            const float ya = (y == y0) ? y - halfWidth : y;
            const float yb = (y == y0) ? y             : y + halfWidth;
            cuboid(xa, ya, z0 - halfWidth, xb, yb, z1 + halfWidth);
        }
    }
}

void buildOutlineGeometry(const sdk::BlockPos& block,
                          const GeometryOptions& opt,
                          std::vector<Line>& outLines,
                          std::vector<Quad>& outQuads) {
    const float x = static_cast<float>(block.x);
    const float y = static_cast<float>(block.y);
    const float z = static_cast<float>(block.z);

    const float gap = std::max(0.0015f, std::min(opt.gap, 0.05f));
    const sdk::Vec3 min  = {x - gap,           y - gap,           z - gap};
    const sdk::Vec3 max  = {x + 1.0f + gap,    y + 1.0f + gap,    z + 1.0f + gap};

    // Mirrors the cpp's policy: slider value maps to a band width by
    // multiplying by 0.01 and clamping to half a block.
    const float thickness =
        std::min(std::max(opt.thickness, 0.0f) * 0.01f, 0.45f);

    if (opt.threeD) {
        const float halfWidth = std::max(thickness, 0.016f);
        outQuads.reserve(outQuads.size() + 72);
        addBoxBeamQuads(outQuads, min, max, halfWidth);

        // 3D mode also gets an outer skeleton line set, expanded by
        // halfWidth so the corners of the beam cage stay connected.
        outLines.reserve(outLines.size() + 12);
        const sdk::Vec3 outerMin  = {
            min.x - halfWidth, min.y - halfWidth, min.z - halfWidth
        };
        const sdk::Vec3 outerMax  = {
            max.x + halfWidth, max.y + halfWidth, max.z + halfWidth
        };
        addBoxLines(outLines, outerMin, outerMax);
    } else {
        outLines.reserve(outLines.size() + 12);
        addBoxLines(outLines, min, max);
        if (thickness > 0.0f) {
            outQuads.reserve(outQuads.size() + 24);
            addThickOutlineQuads(outQuads, min, max, thickness);
        }
    }
}

bool validHitSnapshot(const BlockHitSnapshot& snapshot) {
    const auto finite = [](float value) { return std::isfinite(value); };
    const auto& block = snapshot.blockPos;
    const auto& hit   = snapshot.hitPos;

    return snapshot.valid
        && std::abs(block.x) <= 30000000
        && block.y >= -2048 && block.y <= 2048
        && std::abs(block.z) <= 30000000
        && finite(hit.x) && finite(hit.y) && finite(hit.z)
        && hit.x >= static_cast<float>(block.x) - 1.0f
        && hit.x <= static_cast<float>(block.x) + 2.0f
        && hit.y >= static_cast<float>(block.y) - 1.0f
        && hit.y <= static_cast<float>(block.y) + 2.0f
        && hit.z >= static_cast<float>(block.z) - 1.0f
        && hit.z <= static_cast<float>(block.z) + 2.0f;
}

void hsvToRgb(float h, float s, float v,
              float& outR, float& outG, float& outB) {
    h = std::fmod(h, 1.0f);
    if (h < 0.0f)
        h += 1.0f;
    const float scaled = h * 6.0f;
    const int sector    = static_cast<int>(std::floor(scaled));
    const float fraction = scaled - static_cast<float>(sector);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * fraction);
    const float t = v * (1.0f - s * (1.0f - fraction));
    switch (sector % 6) {
        case 0: outR = v; outG = t; outB = p; break;
        case 1: outR = q; outG = v; outB = p; break;
        case 2: outR = p; outG = v; outB = t; break;
        case 3: outR = p; outG = q; outB = v; break;
        case 4: outR = t; outG = p; outB = v; break;
        default: outR = v; outG = p; outB = q; break;
    }
}

Rgb unpackArgb(uint32_t argb) {
    Rgb out;
    out.r = ((argb >> 16) & 0xFFu) / 255.0f;
    out.g = ((argb >>  8) & 0xFFu) / 255.0f;
    out.b = ((argb      ) & 0xFFu) / 255.0f;
    out.a = ((argb >> 24) & 0xFFu) / 255.0f;
    return out;
}

uint32_t cycleRgbColor(uint32_t baseArgb,
                       bool rgbEnabled,
                       float& hueState,
                       float rgbSpeed,
                       float tick) {
    if (!rgbEnabled)
        return baseArgb;

    // Mirrors the cpp: each render frame advances the hue by
    // 0.002 * rgbSpeed. Pass tick > 0 to multiply by elapsed seconds
    // instead (host / unit-test driver uses that mode).
    const float step = 0.002f * std::max(rgbSpeed, 0.0f);
    const float advance = (tick > 0.0f) ? step * tick : step;
    hueState = std::fmod(hueState + advance, 1.0f);
    if (hueState < 0.0f)
        hueState += 1.0f;

    float r = 1.0f, g = 1.0f, b = 1.0f;
    hsvToRgb(hueState, 1.0f, 1.0f, r, g, b);

    const auto toByte = [](float channel) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f);
    };
    return (baseArgb & 0xFF000000u)
         | (toByte(r) << 16)
         | (toByte(g) <<  8)
         |  toByte(b);
}

} // namespace bedrocktools::blockoutline
