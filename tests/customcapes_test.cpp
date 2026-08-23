// Unit tests for the Custom Capes module's pure helpers (folder scanning,
// the launcher "radio" value format, and cape resampling).
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
        // identity: 64x32 input stays byte-identical
        std::vector<std::uint8_t> src(cc::kCapeWidth * cc::kCapeHeight * 4u);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i % 251);
        const std::vector<std::uint8_t> out = cc::resampleToCape(src.data(), cc::kCapeWidth, cc::kCapeHeight);
        check(out == src, "64x32 input resamples to itself");
    }
    {
        // upscale 2x1 -> 64x32 keeps the two solid color blocks
        const std::uint8_t src[8] = {255, 0, 0, 255, 0, 0, 255, 255};
        const std::vector<std::uint8_t> out = cc::resampleToCape(src, 2, 1);
        check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u, "output is 64x32 RGBA");
        bool leftRed = true, rightBlue = true;
        for (std::uint32_t y = 0; y < cc::kCapeHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeWidth; ++x) {
                const std::size_t i = (y * cc::kCapeWidth + x) * 4u;
                if (x < 32) leftRed &= out[i] == 255 && out[i + 2] == 0;
                else rightBlue &= out[i] == 0 && out[i + 2] == 255;
            }
        }
        check(leftRed && rightBlue, "upscale preserves left/right color blocks");
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
