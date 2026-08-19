// Unit tests for the Crosshair HUD module.
//
// Verifies the module's behavior against the documented contract:
//   * every custom style submits geometry after the cursor renderer fires
//   * nothing is drawn before the vanilla cursor hook fires (freshness gate)
//   * Style::Vanilla forwards to the game's own crosshair renderer
//   * save/load round-trips every setting, including the style radio format
//   * the outline pass is drawn first (dark, thicker) and RGB animates
//
// Unlike the other tests in this directory, the module needs the preloader
// and nlohmann_json headers (normally provided by xmake). Build and run
// standalone (adjust the two package paths to your xmake cache):
//
//     PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//     JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//     g++ -std=c++20 -I include -I src -I "$PRE_LOADER" -I "$JSON" \
//         tests/crosshair_test.cpp -o /tmp/crosshair_test
//     /tmp/crosshair_test

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// Real declarations first so the stubs below match them exactly.
#include "pl/ModMenu.hpp"
#include "pl/memory/Hook.hpp"
#include "bedrocktools/memory/Signatures.hpp"

// ---------------------------------------------------------------------------
// Launcher / memory stubs
// ---------------------------------------------------------------------------

static int g_originalCursorCalls = 0;

namespace pl::memory {
    int hook(FuncPtr, FuncPtr, FuncPtr* originalFunc, HookPriority) {
        if (originalFunc) {
            *originalFunc = (FuncPtr)+[](void*, void*, void*, void*) { ++g_originalCursorCalls; };
        }
        return 0;
    }
    bool unhook(FuncPtr, FuncPtr) { return true; }
}

static std::vector<pl::modmenu::DrawCommand> g_lastCmds;

namespace pl::modmenu {
    bool registerModule(const ModuleInfo&) { return true; }
    void unregisterModule(std::string_view) {}
    void setModuleEnabled(std::string_view, bool) {}
    void submitDrawCommands(std::string_view, std::span<const DrawCommand> commands) {
        g_lastCmds.assign(commands.begin(), commands.end());
    }
    bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
    bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
    bool registerButton(const ButtonInfo&) { return true; }
    void unregisterButton(std::string_view) {}
}

namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) {
        static char fakeTarget[16];
        return (std::uintptr_t)fakeTarget;
    }
}

// The module under test, included directly so its anonymous-namespace pieces
// (the cursor-render detour and the freshness state) are reachable.
#include "modules/hud/crosshair.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

} // namespace

int main() {
    std::printf("crosshair module\n");

    CrosshairModule mod;
    mod.onInit();
    check(mod.customStyleActive(), "default style is a custom one (Cross)");
    mod.setMasterEnabled(true);
    check(mod.enabled, "module enabled");

    // Freshness gate: no cursor render yet -> empty overlay.
    mod.onFrame();
    check(g_lastCmds.empty(), "no crosshair drawn before cursor renderer runs");

    // Simulate the game firing HudCursorRenderer::render.
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    check(g_originalCursorCalls == 0, "vanilla renderer suppressed for custom style");

    // Every custom style submits geometry and stays anchored to the screen
    // center sentinel (all coordinates <= 0 while the module is centered).
    for (int s = 1; s < (int)CrosshairModule::Style::Count; ++s) {
        mod.m_style = (CrosshairModule::Style)s;
        cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
        mod.onFrame();
        char msg[80];
        snprintf(msg, sizeof msg, "style %d draws >= 2 primitives", s);
        check(g_lastCmds.size() >= 2, msg);
        bool centered = true;
        for (const auto& c : g_lastCmds) {
            if (c.x > 0.0f || c.y > 0.0f) centered = false;
        }
        snprintf(msg, sizeof msg, "style %d anchored at screen center", s);
        check(centered, msg);
    }

    // Vanilla style forwards to the original renderer and draws nothing.
    mod.m_style = CrosshairModule::Style::Vanilla;
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    check(g_originalCursorCalls == 1, "vanilla style forwards to original cursor renderer");
    mod.onFrame();
    check(g_lastCmds.empty(), "vanilla style submits no overlay");

    // Config round-trip.
    mod.m_style = CrosshairModule::Style::Scope;
    mod.m_scale = 2.5f; mod.m_thickness = 4.0f; mod.m_opacity = 0.7f;
    mod.m_color = 0xFF123456; mod.m_rgb = true; mod.m_rgbSpeed = 0.8f; mod.m_outline = false;
    nlohmann::json j;
    mod.saveConfig(j);
    check(j["m_style"].get<std::string>().rfind("16,Vanilla,Cross,Dot", 0) == 0, "radio string format");

    CrosshairModule mod2;
    mod2.loadConfig(j);
    check(mod2.m_style == CrosshairModule::Style::Scope, "style round-trip");
    check(mod2.m_scale == 2.5f, "scale round-trip");
    check(mod2.m_thickness == 4.0f, "thickness round-trip");
    check(mod2.m_opacity == 0.7f, "opacity round-trip");
    check((mod2.m_color & 0xFFFFFF) == 0x123456, "color round-trip");
    check(mod2.m_rgb && mod2.m_rgbSpeed == 0.8f && !mod2.m_outline, "rgb/speed/outline round-trip");

    // Radio index parsing variants (menu sends a plain index on changes).
    CrosshairModule mod3;
    nlohmann::json j3;
    mod3.saveConfig(j3);
    j3["m_style"] = "5";
    mod3.loadConfig(j3);
    check(mod3.m_style == CrosshairModule::Style::CircleDot, "plain radio index parses");
    j3["m_style"] = "8,Square Dot,Diamond";
    mod3.loadConfig(j3);
    check(mod3.m_style == CrosshairModule::Style::SquareDot, "index,labels radio parses");
    j3["m_style"] = "99,Bogus";
    mod3.loadConfig(j3);
    check(mod3.m_style == CrosshairModule::Style::SquareDot, "out-of-range index ignored");

    // Outline + RGB layering. mod3 is the module g_crosshairMod points at,
    // matching the production wiring.
    g_lastCursorRenderUs.store(0, std::memory_order_relaxed);
    mod3.setMasterEnabled(true);
    mod3.m_style = CrosshairModule::Style::Cross;
    mod3.m_rgb = true;
    mod3.m_rgbSpeed = 1.0f;
    mod3.m_outline = true;
    for (int frame = 0; frame < 5; ++frame) {
        cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
        mod3.onFrame();
    }
    check(g_lastCmds.size() == 8, "cross = 4 outline lines + 4 color lines");
    check((g_lastCmds[0].color & 0x00FFFFFF) == 0x000000, "first pass is the dark outline");
    check((g_lastCmds[0].color >> 24) != 0, "outline pass keeps nonzero alpha");
    check(((g_lastCmds.back().color >> 24) & 0xFF) == 0xFF, "rgb color pass fully opaque at opacity=1");

    bool animated = false;
    const uint32_t firstColor = g_lastCmds.back().color & 0x00FFFFFF;
    for (int i = 0; i < 40 && !animated; ++i) {
        cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
        mod3.onFrame();
        if ((g_lastCmds.back().color & 0x00FFFFFF) != firstColor) animated = true;
        usleep(3000);
    }
    check(animated, "rgb hue animates over time");

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
