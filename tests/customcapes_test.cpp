// Unit tests for the Custom Capes module's pure helpers (folder scanning,
// the launcher "radio" value format, and the cape layout/resampling —
// including the back-face-only design placement, the flat lining color on
// the inner front face, and the edge strips that give the cape its
// thickness).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/customcapes_test.cpp -o /tmp/customcapes_test
//     /tmp/customcapes_test

#include "modules/player/customcapes_files.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cc = customcapes;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkEqual(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " -> \"" + got + "\" (want \"" + want + "\")");
}

void checkEqual(int got, int want, const std::string& what) {
    check(got == want, what + " -> " + std::to_string(got) + " (want " + std::to_string(want) + ")");
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += '|';
        out += items[i];
    }
    return out;
}

// True when the 64x32-canvas pixels at (ax,ay) and (bx,by) are identical.
bool samePixel(const std::vector<std::uint8_t>& img, std::uint32_t ax, std::uint32_t ay,
               std::uint32_t bx, std::uint32_t by) {
    const std::size_t a = (static_cast<std::size_t>(ay) * cc::kCapeWidth + ax) * 4u;
    const std::size_t b = (static_cast<std::size_t>(by) * cc::kCapeWidth + bx) * 4u;
    return img[a] == img[b] && img[a + 1] == img[b + 1] &&
           img[a + 2] == img[b + 2] && img[a + 3] == img[b + 3];
}

} // namespace

int main() {
    // --- folder scanning -------------------------------------------------
    std::printf("capes folder scan\n");
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "customcapes_test_dir";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "subdir"); // must be ignored

    auto touch = [&](const std::string& name) {
        std::ofstream(dir / name, std::ios::binary) << "x";
    };
    touch("zeta.png");
    touch("alpha.png");
    touch("Mojang.PNG");      // case-insensitive extension
    touch("notes.txt");       // not a png
    touch("weird,comma.png"); // commas break the radio format -> skipped
    touch("alfa.png");

    check(cc::scanCapeFiles("").empty(), "empty directory path yields no files");
    check(cc::scanCapeFiles((dir / "does_not_exist").string()).empty(),
          "missing directory yields no files");
    checkEqual(join(cc::scanCapeFiles(dir.string())),
               "Mojang.PNG|alfa.png|alpha.png|zeta.png",
               "png files only, sorted, no comma names, no subdirs");

    // --- radio serialization ---------------------------------------------
    std::printf("radio value format\n");
    checkEqual(cc::makeRadioValue(0, {}), "0,None", "no files, None selected");
    checkEqual(cc::makeRadioValue(2, {"a.png", "b.png", "c.png"}),
               "2,None,a.png,b.png,c.png", "selection + option list");
    checkEqual(cc::makeRadioValue(-1, {"a.png"}), "0,None,a.png", "negative index clamps to None");
    checkEqual(cc::makeRadioValue(5, {"a.png"}), "1,None,a.png",
               "index past the list clamps to last option");

    // --- radio parsing -----------------------------------------------------
    std::printf("radio value parsing\n");
    int idx = -1;
    std::string name;

    check(cc::parseRadioValue("2,None,a.png,b.png,c.png", idx, name), "full value parses");
    checkEqual(idx, 2, "embedded index");
    checkEqual(name, "b.png", "embedded option name");

    check(cc::parseRadioValue("0,None,a.png", idx, name), "None value parses");
    checkEqual(idx, 0, "None index");
    checkEqual(name, "None", "None name");

    check(cc::parseRadioValue("2", idx, name), "launcher change value parses");
    checkEqual(idx, 2, "plain index from menu");
    checkEqual(name, "", "plain index has no name");

    check(cc::parseRadioValue("a.png", idx, name), "bare file name parses");
    checkEqual(idx, 0, "bare name -> index 0");
    checkEqual(name, "a.png", "bare name recovered");

    check(cc::parseRadioValue("9,None,a.png", idx, name), "oversized index parses");
    checkEqual(idx, 9, "oversized index kept");
    checkEqual(name, "", "oversized index has no embedded name");

    check(!cc::parseRadioValue("", idx, name), "empty value rejected");

    // --- selection resolution ---------------------------------------------
    std::printf("selection resolution\n");
    const std::vector<std::string> files{"a.png", "b.png", "c.png"};
    checkEqual(cc::resolveSelectionIndex(3, "c.png", files), 3, "name and index agree");
    checkEqual(cc::resolveSelectionIndex(1, "c.png", files), 3,
               "stale index fixed via recovered name (list reordered)");
    checkEqual(cc::resolveSelectionIndex(2, "deleted.png", files), 0,
               "deleted file falls back to None");
    checkEqual(cc::resolveSelectionIndex(2, "", files), 2,
               "menu change (no name) stays positional");
    checkEqual(cc::resolveSelectionIndex(0, "None", files), 0, "None stays None");
    checkEqual(cc::resolveSelectionIndex(4, "", files), 0, "positional overflow clamps to None");
    checkEqual(cc::resolveSelectionIndex(-1, "", files), 0, "negative clamps to None");

    // full save -> load round-trip through the picker value
    {
        const std::string saved = cc::makeRadioValue(2, files);
        int rIdx = -1;
        std::string rName;
        cc::parseRadioValue(saved, rIdx, rName);
        checkEqual(cc::resolveSelectionIndex(rIdx, rName, files), 2,
                   "save/load round-trip keeps selection");
    }

    // --- resampling ---------------------------------------------------------
    std::printf("cape resampling\n");
    {
        // identity: an exact 64x32 input is a complete cape canvas and stays
        // byte-identical (full manual control over every face is preserved).
        std::vector<std::uint8_t> src(cc::kCapeWidth * cc::kCapeHeight * 4u);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i % 251);
        const std::vector<std::uint8_t> out = cc::resampleToCape(src.data(), cc::kCapeWidth, cc::kCapeHeight);
        check(out == src, "64x32 input resamples to itself");
    }
    {
        // 2x1 red|blue source: the design lands on the OUTER BACK face only,
        // the box (1,1)..(11,17) on the 64x32 canvas.
        const std::uint8_t src[8] = {255, 0, 0, 255, 0, 0, 255, 255};
        const std::vector<std::uint8_t> out = cc::resampleToCape(src, 2, 1);
        check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u, "output is 64x32 RGBA");

        bool backBlocks = true;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth +
                     cc::kCapeBackX + x) * 4u;
                // nearest-neighbor: left half of the design <- red source pixel
                const bool red = x < cc::kCapeBackWidth / 2;
                backBlocks &= red ? (out[i] == 255 && out[i + 1] == 0 && out[i + 2] == 0)
                                  : (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 255);
            }
        }
        check(backBlocks, "design is scaled onto the back face box (1,1)..(11,17)");

        // inner front face: one flat lining color, never the design. The
        // average of (255,0,0,255) and (0,0,255,255) over the design is
        // (127,0,127,255); halving RGB gives (63,0,63,255).
        bool frontUniform = true, frontIsLiner = true, frontNoDesign = true;
        const std::size_t first =
            (static_cast<std::size_t>(cc::kCapeFrontY) * cc::kCapeWidth + cc::kCapeFrontX) * 4u;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeFrontY + y) * cc::kCapeWidth +
                     cc::kCapeFrontX + x) * 4u;
                frontIsLiner &= out[i] == 63 && out[i + 1] == 0 &&
                                out[i + 2] == 63 && out[i + 3] == 255;
                frontNoDesign &= !samePixel(out, cc::kCapeFrontX + x, cc::kCapeFrontY + y,
                                            cc::kCapeBackX + x, cc::kCapeBackY + y);
                frontNoDesign &= !(out[i] == 255 && out[i + 1] == 0 && out[i + 2] == 0 &&
                                   out[i + 3] == 255);
                frontNoDesign &= !(out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 255 &&
                                   out[i + 3] == 255);
                frontUniform &= out[i] == out[first] && out[i + 1] == out[first + 1] &&
                                out[i + 2] == out[first + 2] && out[i + 3] == out[first + 3];
            }
        }
        check(frontIsLiner, "front face is the flat lining color (63,0,63,255)");
        check(frontUniform, "front face is a single uniform color");
        check(frontNoDesign, "front face never repeats the design pixels");

        // edges: top/bottom strips carry the design's top/bottom rows, the
        // side strips carry the design's left/right columns.
        bool edgesFromImage = true;
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
            const bool red = x < cc::kCapeBackWidth / 2;
            const std::size_t top =
                (static_cast<std::size_t>(cc::kCapeTopY) * cc::kCapeWidth + cc::kCapeTopX + x) * 4u;
            const std::size_t bottom =
                (static_cast<std::size_t>(cc::kCapeBottomY) * cc::kCapeWidth +
                 cc::kCapeBottomX + x) * 4u;
            edgesFromImage &= red ? (out[top] == 255 && out[top + 2] == 0)
                                  : (out[top] == 0 && out[top + 2] == 255);
            edgesFromImage &= red ? (out[bottom] == 255 && out[bottom + 2] == 0)
                                  : (out[bottom] == 0 && out[bottom + 2] == 255);
        }
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            const std::size_t sideR =
                (static_cast<std::size_t>(cc::kCapeSideY + y) * cc::kCapeWidth +
                 cc::kCapeSideRightX) * 4u;
            const std::size_t sideL =
                (static_cast<std::size_t>(cc::kCapeSideY + y) * cc::kCapeWidth +
                 cc::kCapeSideLeftX) * 4u;
            edgesFromImage &= out[sideR] == 255 && out[sideR + 2] == 0; // design left column is red
            edgesFromImage &= out[sideL] == 0 && out[sideL + 2] == 255; // right column is blue
        }
        check(edgesFromImage, "top/bottom/side strips carry the design edge colors");

        // everything outside the cape layout stays transparent
        bool outsideTransparent = true;
        for (std::uint32_t y = 0; y < cc::kCapeHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeWidth; ++x) {
                const bool used =
                    (y == cc::kCapeTopY &&
                     x >= cc::kCapeTopX && x < cc::kCapeTopX + cc::kCapeBackWidth) ||
                    (y == cc::kCapeBottomY &&
                     x >= cc::kCapeBottomX && x < cc::kCapeBottomX + cc::kCapeBackWidth) ||
                    (y >= cc::kCapeSideY && y < cc::kCapeSideY + cc::kCapeBackHeight &&
                     x <= cc::kCapeSideRightX) ||
                    (y >= cc::kCapeFrontY && y < cc::kCapeFrontY + cc::kCapeBackHeight &&
                     x >= cc::kCapeSideRightX &&
                     x < cc::kCapeFrontX + cc::kCapeBackWidth);
                if (used) continue;
                const std::size_t i = (static_cast<std::size_t>(y) * cc::kCapeWidth + x) * 4u;
                outsideTransparent &= out[i] == 0 && out[i + 1] == 0 &&
                                      out[i + 2] == 0 && out[i + 3] == 0;
            }
        }
        check(outsideTransparent, "canvas outside the cape layout stays transparent");
    }
    {
        // exact edge continuation: a 10x16 source maps 1:1 onto the back
        // face, so every edge-strip pixel must equal its adjacent back-face
        // edge pixel exactly.
        std::vector<std::uint8_t> src(cc::kCapeBackWidth * cc::kCapeBackHeight * 4u);
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * cc::kCapeBackWidth + x) * 4u;
                src[i + 0] = static_cast<std::uint8_t>(x * 25);         // unique per column
                src[i + 1] = static_cast<std::uint8_t>(y * 16 + 1);     // unique per row
                src[i + 2] = 200;
                src[i + 3] = 255;
            }
        }
        const std::vector<std::uint8_t> out =
            cc::resampleToCape(src.data(), cc::kCapeBackWidth, cc::kCapeBackHeight);
        bool exact = true;
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
            exact &= samePixel(out, cc::kCapeTopX + x, cc::kCapeTopY,
                               cc::kCapeBackX + x, cc::kCapeBackY);
            exact &= samePixel(out, cc::kCapeBottomX + x, cc::kCapeBottomY,
                               cc::kCapeBackX + x, cc::kCapeBackY + cc::kCapeBackHeight - 1);
        }
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            exact &= samePixel(out, cc::kCapeSideRightX, cc::kCapeSideY + y,
                               cc::kCapeBackX, cc::kCapeBackY + y);
            exact &= samePixel(out, cc::kCapeSideLeftX, cc::kCapeSideY + y,
                               cc::kCapeBackX + cc::kCapeBackWidth - 1, cc::kCapeBackY + y);
        }
        check(exact, "edge strips exactly copy the adjacent back-face edge pixels");
    }
    {
        // degenerate input never crashes and stays transparent-black
        const std::vector<std::uint8_t> out = cc::resampleToCape(nullptr, 0, 0);
        check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u, "null input still yields canvas");
        bool allZero = true;
        for (std::uint8_t b : out) allZero &= b == 0;
        check(allZero, "null input is transparent");
    }

    std::filesystem::remove_all(dir, ec);

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes tests passed" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
