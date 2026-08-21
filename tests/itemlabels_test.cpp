// Unit tests for the Item Labels module.
//
// Verifies the label formatting contract:
//   * name + " xN" count suffix with the vanilla gray color code
//   * prefix / suffix placement, bold and italic style codes
//   * multiline mode splits count+suffix onto a second line
//   * "#RRGGBB" and "#AARRGGBB" colors map to the closest vanilla code
//   * save/load round-trips every setting (including the mode radio format)
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I include -I src -I <nlohmann-json-include> \
//         tests/itemlabels_test.cpp -o /tmp/itemlabels_test
//     /tmp/itemlabels_test

#include "modules/visual/itemlabels.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/events/EventBus.hpp"

// ---------------------------------------------------------------------------
// Stubs for the game-facing symbols the module references but the test never
// invokes (signature resolution and the event bus).
// ---------------------------------------------------------------------------
namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) { return 0; }
    void clear() {}
}

namespace bedrocktools::events {
    EventBus& bus() {
        static EventBus instance;
        return instance;
    }
}

namespace {

int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

const char* kSection = "\xC2\xA7";

std::string label(const std::string& name, int count) {
    return ItemLabelsModule::buildLabel(name, count, /*showCount*/ true,
                                        /*multiline*/ false, /*bold*/ false,
                                        /*italic*/ false, "", "", "#FFFFFF");
}

void testBasicFormatting() {
    // Diamond x3, white, no extras.
    std::string l = label("Diamond", 3);
    CHECK(l == std::string(kSection) + "fDiamond" + kSection + "7x3");

    // Count hidden.
    l = ItemLabelsModule::buildLabel("Diamond", 64, false, false, false, false,
                                     "", "", "#FFFFFF");
    CHECK(l == std::string(kSection) + "fDiamond");

    // Count of one still renders.
    l = label("Stick", 1);
    CHECK(l == std::string(kSection) + "fStick" + kSection + "7x1");
}

void testPrefixSuffixAndStyle() {
    std::string l = ItemLabelsModule::buildLabel(
        "Iron Ingot", 5, true, false, true, true, "[!] ", " !!", "#FFFFFF");
    CHECK(l == "[!] " + std::string(kSection) + "f" + kSection + "l" +
                    kSection + "oIron Ingot" + kSection + "7x5 !!");

    // Suffix only, no count.
    l = ItemLabelsModule::buildLabel("Apple", 2, false, false, false, false,
                                     "", "yum", "#FFFFFF");
    CHECK(l == std::string(kSection) + "fAppleyum");
}

void testMultiline() {
    std::string l = ItemLabelsModule::buildLabel(
        "Diamond", 4, true, true, false, false, "", "", "#FFFFFF");
    CHECK(l == std::string(kSection) + "fDiamond\n" +
                    std::string(kSection) + "f" + kSection + "7x4");

    // No count and no suffix -> stays a single line.
    l = ItemLabelsModule::buildLabel("Diamond", 1, false, true, false, false,
                                     "", "", "#FFFFFF");
    CHECK(l == std::string(kSection) + "fDiamond");
}

void testColorMapping() {
    // White stays white, red maps to 'c', pure blue is closest to dark blue
    // '1' (vanilla has no pure-blue code).
    CHECK(label("X", 1) == std::string(kSection) + "fX" + kSection + "7x1");
    CHECK(ItemLabelsModule::buildLabel("X", 1, true, false, false, false, "", "",
                                       "#FF5555") ==
          std::string(kSection) + "cX" + kSection + "7x1");
    CHECK(ItemLabelsModule::buildLabel("X", 1, true, false, false, false, "", "",
                                       "#0000FF") ==
          std::string(kSection) + "1X" + kSection + "7x1");

    // #AARRGGBB input drops the alpha.
    CHECK(ItemLabelsModule::buildLabel("X", 1, true, false, false, false, "", "",
                                       "#FF00FF00") ==
          std::string(kSection) + "2X" + kSection + "7x1");

    // Garbage input falls back to white.
    CHECK(ItemLabelsModule::buildLabel("X", 1, true, false, false, false, "", "",
                                       "nope") ==
          std::string(kSection) + "fX" + kSection + "7x1");
}

void testConfigRoundTrip() {
    ItemLabelsModule mod;
    mod.m_mode = 1;
    mod.m_maxDistance = 48.0f;
    mod.m_maxItems = 12;
    mod.m_occlusion = false;
    mod.m_showCount = false;
    mod.m_multiline = true;
    mod.m_bold = true;
    mod.m_italic = true;
    mod.m_prefix = "pre ";
    mod.m_suffix = " suf";
    mod.m_textColor = "#FF0000";

    nlohmann::json j;
    mod.saveConfig(j);

    CHECK(j["m_mode"].is_string());
    CHECK(j["m_mode"].get<std::string>() == "1,Nearest,Crosshair");
    CHECK(j["m_maxDistance"].get<float>() == 48.0f);
    CHECK(j["m_maxItems"].get<int>() == 12);
    CHECK(j["m_occlusion"].get<bool>() == false);
    CHECK(j["m_showCount"].get<bool>() == false);
    CHECK(j["m_multiline"].get<bool>() == true);
    CHECK(j["m_bold"].get<bool>() == true);
    CHECK(j["m_italic"].get<bool>() == true);
    CHECK(j["m_prefix"].get<std::string>() == "pre ");
    CHECK(j["m_suffix"].get<std::string>() == " suf");
    CHECK(j["m_textColor"].get<std::string>() == "#FF0000");

    // Mode radio parses back.
    ItemLabelsModule restored;
    restored.loadConfig(j);
    CHECK(restored.m_mode == 1);
    CHECK(restored.m_maxDistance == 48.0f);
    CHECK(restored.m_maxItems == 12);
    CHECK(restored.m_occlusion == false);
    CHECK(restored.m_showCount == false);
    CHECK(restored.m_multiline == true);
    CHECK(restored.m_bold == true);
    CHECK(restored.m_italic == true);
    CHECK(restored.m_prefix == "pre ");
    CHECK(restored.m_suffix == " suf");
    CHECK(restored.m_textColor == "#FF0000");

    // Plain-integer mode configs (older menu format) are accepted too.
    nlohmann::json j2;
    j2["m_mode"] = 0;
    ItemLabelsModule restored2;
    restored2.loadConfig(j2);
    CHECK(restored2.m_mode == 0);
}

}  // namespace

int main() {
    testBasicFormatting();
    testPrefixSuffixAndStyle();
    testMultiline();
    testColorMapping();
    testConfigRoundTrip();

    if (g_failures == 0) {
        std::printf("itemlabels: all tests passed\n");
        return 0;
    }
    std::printf("itemlabels: %d test(s) failed\n", g_failures);
    return 1;
}
