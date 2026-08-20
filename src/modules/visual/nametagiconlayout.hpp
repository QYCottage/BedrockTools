#pragma once

// ---------------------------------------------------------------------------
// Placement math for the Third Person Nametag module's "Nametag Icon".
//
// The icon used to be a fixed-size billboard floating above the player's head,
// which never lined up with the nametag the module restores. It is now placed
// on the LEFT side of the name, like a rank/badge icon:
//
//        [icon]  PlayerName
//
// Everything here works in screen pixels and is deliberately free of engine
// dependencies so it can be unit tested on the host.
//
// How the numbers are derived:
//
//   * Bedrock draws nametags as world-space billboards, so their on-screen
//     size shrinks with distance. A nametag line is ~0.25 blocks tall in world
//     space; projecting that height through the camera gives the text height in
//     pixels (nametagTextHeightPx).
//   * The Minecraft font is a fixed-advance bitmap font: a glyph advance is
//     ~6px for a ~10px line, i.e. 0.6 * line height (kGlyphAdvanceRatio).
//     Multiplying by the visible character count gives the text width, and the
//     game adds a small padding around the dark background box.
//   * The icon is squared, sized relative to the text height so it scales with
//     the nametag, and pushed left of the name's left edge by a small gap.
//
// The result is that the icon tracks the nametag at any distance and camera
// angle instead of being pinned to a fixed pixel size above the head.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstddef>
#include <string_view>

namespace bedrocktools::nametag {

// World-space height of a single nametag line, in blocks.
inline constexpr float kNametagLineWorldHeight = 0.25f;

// Minecraft font glyph advance relative to the line height.
inline constexpr float kGlyphAdvanceRatio = 0.6f;

// Padding the game's nametag background box adds on each side of the text,
// relative to the text height.
inline constexpr float kNametagPaddingRatio = 0.3f;

// Icon edge length relative to the nametag text height (slightly taller than
// the text so the logo reads as a badge rather than a glyph).
inline constexpr float kIconSizeRatio = 1.4f;

// Gap between the icon and the left edge of the nametag box, relative to the
// text height.
inline constexpr float kIconGapRatio = 0.35f;

// Clamp for the on-screen icon size so it never disappears at long range or
// swallows the screen right in front of the camera, in pixels.
inline constexpr float kMinIconSizePx = 6.0f;
inline constexpr float kMaxIconSizePx = 512.0f;

struct IconRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct IconPlacement {
    float nameCenterX = 0.0f;  // projected center of the nametag, in pixels
    float nameCenterY = 0.0f;  // vertical center of the nametag text, in pixels
    float textHeightPx = 0.0f; // on-screen height of one nametag line
    int nameLength = 0;        // visible characters of the displayed name
    float scale = 1.0f;        // user-facing size multiplier
};

// Number of characters the game actually paints: UTF-8 code points with the
// "§x" formatting codes removed (they are consumed by the font renderer).
inline int visibleNameLength(std::string_view name) {
    int count = 0;
    for (std::size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);

        // Section sign, either as UTF-8 (0xC2 0xA7) or as a raw 0xA7 byte,
        // followed by the format character.
        if (c == 0xC2 && i + 1 < name.size() && static_cast<unsigned char>(name[i + 1]) == 0xA7) {
            i += 2;
            while (i < name.size() && (static_cast<unsigned char>(name[i]) & 0xC0) == 0x80) ++i;
            if (i < name.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += 1;
            if (i < name.size()) ++i;
            continue;
        }

        if (c == '\n' || c == '\r') { ++i; continue; }

        // Advance one UTF-8 code point.
        std::size_t length = 1;
        if ((c & 0xE0) == 0xC0) length = 2;
        else if ((c & 0xF0) == 0xE0) length = 3;
        else if ((c & 0xF8) == 0xF0) length = 4;
        i += length;
        ++count;
    }
    return count;
}

// On-screen height of one nametag line for a billboard `distance` blocks away.
// `verticalFov` is in radians and `screenHeight` in pixels.
inline float nametagTextHeightPx(float distance, float screenHeight, float verticalFov) {
    if (distance <= 0.01f || screenHeight <= 0.0f) return 0.0f;
    const float halfExtent = std::tan(verticalFov * 0.5f) * distance;  // world units per half screen
    if (halfExtent <= 1e-6f) return 0.0f;
    return kNametagLineWorldHeight / (2.0f * halfExtent) * screenHeight;
}

// Width of the drawn name, in pixels (text only, without the box padding).
inline float nametagTextWidthPx(int nameLength, float textHeightPx) {
    const int glyphs = nameLength > 0 ? nameLength : 1;
    return static_cast<float>(glyphs) * kGlyphAdvanceRatio * textHeightPx;
}

// Rectangle for the icon sitting immediately left of the player's name.
inline IconRect iconRectLeftOfName(const IconPlacement& placement) {
    IconRect rect;
    if (placement.textHeightPx <= 0.0f) return rect;

    float size = placement.textHeightPx * kIconSizeRatio * (placement.scale > 0.0f ? placement.scale : 1.0f);
    if (size < kMinIconSizePx) size = kMinIconSizePx;
    if (size > kMaxIconSizePx) size = kMaxIconSizePx;

    const float halfName = nametagTextWidthPx(placement.nameLength, placement.textHeightPx) * 0.5f;
    const float boxLeft = placement.nameCenterX - halfName - placement.textHeightPx * kNametagPaddingRatio;

    rect.w = size;
    rect.h = size;
    rect.x = boxLeft - placement.textHeightPx * kIconGapRatio - size;
    rect.y = placement.nameCenterY - size * 0.5f;
    return rect;
}

} // namespace bedrocktools::nametag
