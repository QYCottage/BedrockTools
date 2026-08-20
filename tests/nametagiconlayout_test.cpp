// Unit tests for the Nametag Icon placement math (Third Person Nametag).
//
// The module draws the launcher icon to the LEFT of the player's name, so the
// contract verified here is:
//   * the icon never overlaps the name: its right edge stays left of the
//     nametag box, with a gap
//   * it is vertically centered on the nametag line
//   * it shrinks/grows with the nametag (world-space billboard projection)
//   * longer names push the icon further left
//   * the size multiplier scales the icon without moving it onto the name
//   * formatting codes (§a) and multi-byte characters are counted the way the
//     font renderer paints them
//
// Build and run standalone (no engine headers needed):
//
//     g++ -std=c++20 -I src tests/nametagiconlayout_test.cpp -o /tmp/t && /tmp/t

#include "modules/visual/nametagiconlayout.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace nt = bedrocktools::nametag;

static int g_failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static void checkNear(float actual, float expected, float tolerance, const char* what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) {
        std::printf("FAIL: %s (expected %.3f, got %.3f)\n", what, expected, actual);
        ++g_failures;
    } else {
        std::printf("ok: %s (%.3f)\n", what, actual);
    }
}

static nt::IconPlacement placementFor(int nameLength, float textHeight = 20.0f, float scale = 1.0f) {
    nt::IconPlacement placement;
    placement.nameCenterX = 960.0f;
    placement.nameCenterY = 400.0f;
    placement.textHeightPx = textHeight;
    placement.nameLength = nameLength;
    placement.scale = scale;
    return placement;
}

int main() {
    // --- visible length -----------------------------------------------------
    check(nt::visibleNameLength("Steve") == 5, "plain ASCII name is counted per character");
    check(nt::visibleNameLength("\xC2\xA7" "aSteve") == 5, "UTF-8 section formatting codes are skipped");
    check(nt::visibleNameLength("\xA7" "cSteve" "\xA7" "r") == 5, "raw 0xA7 formatting codes are skipped");
    check(nt::visibleNameLength("Ährlich") == 7, "multi-byte characters count once");
    check(nt::visibleNameLength("") == 0, "empty name has no visible characters");

    // --- projection ---------------------------------------------------------
    const float fov = 70.0f * 3.14159265f / 180.0f;
    const float near4 = nt::nametagTextHeightPx(4.0f, 1080.0f, fov);
    const float far16 = nt::nametagTextHeightPx(16.0f, 1080.0f, fov);
    check(near4 > 0.0f, "a nametag 4 blocks away has a positive on-screen height");
    checkNear(far16, near4 / 4.0f, 0.01f, "billboard height is inversely proportional to distance");
    check(nt::nametagTextHeightPx(0.0f, 1080.0f, fov) == 0.0f, "degenerate distance yields no height");
    check(nt::nametagTextHeightPx(4.0f, 0.0f, fov) == 0.0f, "degenerate screen height yields no height");

    // --- basic placement ----------------------------------------------------
    {
        const auto placement = placementFor(5);
        const nt::IconRect rect = nt::iconRectLeftOfName(placement);
        const float nameHalf = nt::nametagTextWidthPx(5, placement.textHeightPx) * 0.5f;

        check(rect.w > 0.0f && rect.w == rect.h, "the icon is a non-empty square");
        check(rect.x + rect.w < placement.nameCenterX - nameHalf,
              "the icon sits fully to the left of the name");
        checkNear(rect.x + rect.w,
                  placement.nameCenterX - nameHalf
                      - placement.textHeightPx * (nt::kNametagPaddingRatio + nt::kIconGapRatio),
                  0.01f,
                  "there is a padding+gap between the icon and the nametag box");
        checkNear(rect.y + rect.h * 0.5f, placement.nameCenterY, 0.01f,
                  "the icon is vertically centered on the name");
        checkNear(rect.h, placement.textHeightPx * nt::kIconSizeRatio, 0.01f,
                  "the icon is sized relative to the text height");
    }

    // --- reacts to the name length -----------------------------------------
    {
        const nt::IconRect shortName = nt::iconRectLeftOfName(placementFor(3));
        const nt::IconRect longName = nt::iconRectLeftOfName(placementFor(16));
        check(longName.x < shortName.x, "a longer name pushes the icon further left");
        checkNear(shortName.x - longName.x,
                  nt::nametagTextWidthPx(16, 20.0f) * 0.5f - nt::nametagTextWidthPx(3, 20.0f) * 0.5f,
                  0.01f,
                  "the shift equals half of the extra text width");
        const nt::IconRect noName = nt::iconRectLeftOfName(placementFor(0));
        check(noName.x + noName.w < 960.0f, "an unknown name still keeps the icon left of center");
    }

    // --- reacts to distance -------------------------------------------------
    {
        auto close = placementFor(6, nt::nametagTextHeightPx(4.0f, 1080.0f, fov));
        auto distant = placementFor(6, nt::nametagTextHeightPx(20.0f, 1080.0f, fov));
        const nt::IconRect closeRect = nt::iconRectLeftOfName(close);
        const nt::IconRect distantRect = nt::iconRectLeftOfName(distant);
        check(distantRect.w < closeRect.w, "the icon shrinks with distance, like the nametag");
        check(distantRect.x > closeRect.x, "a distant nametag is narrower, so the icon moves inward");
        check(distantRect.x + distantRect.w < distant.nameCenterX,
              "the icon stays on the left side at range");
    }

    // --- size multiplier ----------------------------------------------------
    {
        const nt::IconRect normal = nt::iconRectLeftOfName(placementFor(6, 20.0f, 1.0f));
        const nt::IconRect big = nt::iconRectLeftOfName(placementFor(6, 20.0f, 2.0f));
        checkNear(big.w, normal.w * 2.0f, 0.01f, "the scale option doubles the icon size");
        checkNear(big.x + big.w, normal.x + normal.w, 0.01f,
                  "scaling grows the icon away from the name, never over it");
        checkNear(big.y + big.h * 0.5f, normal.y + normal.h * 0.5f, 0.01f,
                  "scaling keeps the icon centered on the name");

        const nt::IconRect huge = nt::iconRectLeftOfName(placementFor(6, 20.0f, 1000.0f));
        check(huge.w <= nt::kMaxIconSizePx, "the icon size is clamped from above");
        const nt::IconRect tiny = nt::iconRectLeftOfName(placementFor(6, 0.5f, 0.1f));
        check(tiny.w >= nt::kMinIconSizePx, "the icon size is clamped from below");
    }

    // --- degenerate input ---------------------------------------------------
    {
        const nt::IconRect none = nt::iconRectLeftOfName(placementFor(6, 0.0f));
        check(none.w == 0.0f && none.h == 0.0f, "no text height means nothing to draw");
    }

    std::printf("\n%s\n", g_failures == 0 ? "nametagiconlayout: all checks passed"
                                          : "nametagiconlayout: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
