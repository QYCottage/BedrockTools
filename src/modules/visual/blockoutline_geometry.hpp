#pragma once

#include <array>

namespace bedrocktools::modules::blockoutline {

struct Point {
    float x;
    float y;
    float z;

    constexpr bool operator==(const Point&) const = default;
};

struct Line {
    Point from;
    Point to;
};

struct Quad {
    Point a;
    Point b;
    Point c;
    Point d;
};

// Builds the twelve edges of a block-sized axis-aligned box.  Keeping this
// geometry independent from Minecraft makes it easy to verify on the host and
// avoids rebuilding edge topology every frame.
constexpr std::array<Line, 12> makeBox(float x, float y, float z, float expand = 0.002f) {
    const float x0 = x - expand;
    const float y0 = y - expand;
    const float z0 = z - expand;
    const float x1 = x + 1.0f + expand;
    const float y1 = y + 1.0f + expand;
    const float z1 = z + 1.0f + expand;

    return {{
        // Bottom face.
        {{x0, y0, z0}, {x1, y0, z0}},
        {{x1, y0, z0}, {x1, y0, z1}},
        {{x1, y0, z1}, {x0, y0, z1}},
        {{x0, y0, z1}, {x0, y0, z0}},
        // Top face.
        {{x0, y1, z0}, {x1, y1, z0}},
        {{x1, y1, z0}, {x1, y1, z1}},
        {{x1, y1, z1}, {x0, y1, z1}},
        {{x0, y1, z1}, {x0, y1, z0}},
        // Vertical edges.
        {{x0, y0, z0}, {x0, y1, z0}},
        {{x1, y0, z0}, {x1, y1, z0}},
        {{x1, y0, z1}, {x1, y1, z1}},
        {{x0, y0, z1}, {x0, y1, z1}},
    }};
}

// Builds the six faces of a block-sized axis-aligned box as quads. Used by the
// "3D" rendering mode to fill the box with a translucent overlay so the block
// reads as a solid volume instead of a bare wireframe.
constexpr std::array<Quad, 6> makeFaces(float x, float y, float z, float expand = 0.0f) {
    const float x0 = x - expand;
    const float y0 = y - expand;
    const float z0 = z - expand;
    const float x1 = x + 1.0f + expand;
    const float y1 = y + 1.0f + expand;
    const float z1 = z + 1.0f + expand;

    return {{
        // -Y (bottom).
        {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},
        // +Y (top).
        {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}},
        // -Z.
        {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}},
        // +Z.
        {{x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1}, {x1, y0, z1}},
        // -X.
        {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},
        // +X.
        {{x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0}},
    }};
}

} // namespace bedrocktools::modules::blockoutline
