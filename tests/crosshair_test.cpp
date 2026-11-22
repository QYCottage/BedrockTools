// Unit tests for the Crosshair HUD module.
//
// Verifies the module's behavior against the documented contract:
//   * every custom style submits geometry after the cursor renderer fires
//   * nothing is drawn before the vanilla cursor hook fires (freshness gate)
//   * Style::Vanilla forwards to the game's own crosshair renderer
//   * every style draws its own shape, inside the size budget, and scales
//     linearly with the Size option
//   * the style radio is generated from the style table, append-only so old
//     configs keep resolving to the same shape
//   * save/load round-trips every setting, including the style radio format
//   * the outline pass is drawn first (dark, thicker) and RGB animates
//   * the hit indicator recolors a custom crosshair without Hitbox enabled
//   * Vanilla + indicator falls back to an overlay when tinting is impossible
//   * the show-in-third-person option suppresses/restores third-person draw
//
// Unlike the other tests in this directory, the module needs the preloader
// API types and the nlohmann_json headers. The preloader types come from
// tests/crosshair_fakepl (full preloader headers would redeclare the stubs
// below), and JSON/JNI come from tests/fakejson and tests/fakejni. Build and
// run standalone:
//
//     g++ -std=c++20 -I include -I src \
//         -I tests/crosshair_fakepl -I tests/fakejson -I tests/fakejni \
//         tests/crosshair_test.cpp -o /tmp/crosshair_test
//     /tmp/crosshair_test

#include <algorithm>
#include <cmath>
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
#include "bedrocktools/events/EventBus.hpp"

// ---------------------------------------------------------------------------
// Launcher / memory stubs
// ---------------------------------------------------------------------------

static int g_originalCursorCalls = 0;

namespace pl::memory {
    // The real preloader API takes a HookPriority with a default (Normal),
    // which is also what bedrocktools::hooks::install relies on when it calls
    // pl::memory::hook with only three arguments.
    int hook(FuncPtr, FuncPtr, FuncPtr* originalFunc, HookPriority = HookPriority::Normal) {
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

namespace bedrocktools::events {
    EventBus& bus() {
        static EventBus instance;
        return instance;
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

// Builds one style without the overlay pipeline: the center is the origin and
// the scale is 1, so the coordinates below are plain pixel offsets from the
// aim point and can be compared against the documented shape metrics.
std::vector<PLModMenu_DrawCommand> shapeFor(CrosshairModule::Style style, float scale = 1.0f) {
    std::vector<PLModMenu_DrawCommand> cmds;
    ShapePainter painter{cmds, 0.0f, 0.0f, 2.0f, 0xFF112233u};
    buildShape(style, painter, scale);
    return cmds;
}

// Shortest distance from the shape's center to a drawn primitive. Lines are
// measured to their segment, filled rects (dots) to their nearest corner, so a
// "center stays clear" check cannot be fooled by a primitive whose start point
// is close while its body runs through the middle.
float distanceToCenter(const PLModMenu_DrawCommand& c) {
    if (c.type != PL_DRAW_LINE) {
        float best = 1e9f;
        const float corners[4][2] = {
            {c.x, c.y}, {c.x + c.w, c.y}, {c.x, c.y + c.h}, {c.x + c.w, c.y + c.h}
        };
        for (const auto& corner : corners) {
            const float dx = corner[0] < 0.0f ? -corner[0] : corner[0];
            const float dy = corner[1] < 0.0f ? -corner[1] : corner[1];
            // A rect that straddles the origin has a negative coordinate on
            // one side, which puts the center inside it.
            if ((c.x <= 0.0f && c.x + c.w >= 0.0f) && (c.y <= 0.0f && c.y + c.h >= 0.0f)) return 0.0f;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < best) best = len;
        }
        return best;
    }

    const float x2 = c.x + c.w;
    const float y2 = c.y + c.h;
    const float vx = x2 - c.x;
    const float vy = y2 - c.y;
    const float lenSq = vx * vx + vy * vy;
    float t = lenSq > 1e-6f ? -(c.x * vx + c.y * vy) / lenSq : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float px = c.x + vx * t;
    const float py = c.y + vy * t;
    return std::sqrt(px * px + py * py);
}

// Identity of a shape: type plus geometry of every primitive it submits.
// Two styles with the same signature are the same crosshair.
std::string shapeSignature(const std::vector<PLModMenu_DrawCommand>& cmds) {
    std::string signature;
    char part[64];
    for (const auto& c : cmds) {
        std::snprintf(part, sizeof part, "%d:%.2f,%.2f,%.2f,%.2f|", static_cast<int>(c.type),
                      c.x, c.y, c.w, c.h);
        signature += part;
    }
    return signature;
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

    // -----------------------------------------------------------------------
    // Shape geometry. Each custom style must draw something, must not collide
    // with another style, must keep its size budget and - for the gapped
    // reticles - must leave the exact aim point free.
    // -----------------------------------------------------------------------
    std::vector<std::string> signatures;
    bool allDraw = true;
    bool allDistinct = true;
    bool allInBox = true;
    for (int s = 1; s < (int)CrosshairModule::Style::Count; ++s) {
        const auto cmds = shapeFor((CrosshairModule::Style)s);
        if (cmds.empty()) allDraw = false;
        const std::string signature = shapeSignature(cmds);
        if (std::find(signatures.begin(), signatures.end(), signature) != signatures.end()) {
            allDistinct = false;
        }
        signatures.push_back(signature);
        for (const auto& c : cmds) {
            if (std::fabs(c.x) > 26.0f || std::fabs(c.y) > 26.0f ||
                std::fabs(c.x + c.w) > 26.0f || std::fabs(c.y + c.h) > 26.0f) {
                allInBox = false;
            }
        }
    }
    check(allDraw, "every custom style submits geometry");
    check(signatures.size() == (size_t)((int)CrosshairModule::Style::Count - 1),
          "one shape per custom style value");
    check(allDistinct, "no two styles draw the same shape");
    check(allInBox, "every shape stays within 26 px of the center at scale 1");
    check(shapeFor(CrosshairModule::Style::Vanilla).empty(), "vanilla style has no custom geometry");

    struct ShapeCheck {
        CrosshairModule::Style style;
        size_t commands;
        const char* what;
    };
    const ShapeCheck kShapeChecks[] = {
        {CrosshairModule::Style::Cross, 4, "Cross keeps its four arms"},
        {CrosshairModule::Style::CrossX, 8, "Cross X is four axis arms plus four diagonals"},
        {CrosshairModule::Style::Vertical, 2, "Vertical is the top and bottom arm only"},
        {CrosshairModule::Style::Horizontal, 2, "Horizontal is the left and right arm only"},
        {CrosshairModule::Style::TShapeDown, 2, "T Shape Down is a stem plus a bottom bar"},
        {CrosshairModule::Style::Brackets, 8, "Brackets is one L per corner"},
        {CrosshairModule::Style::BracketsDot, 9, "Brackets Dot adds the center dot"},
        {CrosshairModule::Style::Target, 41, "Target is two rings plus the center dot"},
        {CrosshairModule::Style::RingTicks, 28, "Ring Ticks is a ring plus four outward ticks"},
        {CrosshairModule::Style::BrokenRing, 40, "Broken Ring is four ten-segment arcs"},
        {CrosshairModule::Style::Triangle, 3, "Triangle is three closed edges"},
        {CrosshairModule::Style::Grid, 4, "Grid is two lines per axis"},
        {CrosshairModule::Style::MilDot, 12, "Mil Dots is four arms plus eight dots"},
        {CrosshairModule::Style::Converge, 8, "Converge is four inward chevrons"},
        {CrosshairModule::Style::SquarePlusDot, 13, "Square Plus Dot is brackets plus a gapped cross plus the center dot"},
    };
    for (const auto& expected : kShapeChecks) {
        check(shapeFor(expected.style).size() == expected.commands, expected.what);
    }

    // The gapped styles exist to keep the target unobstructed, so no primitive
    // of theirs may reach into the center gap.
    const CrosshairModule::Style kGapped[] = {
        CrosshairModule::Style::Cross,   CrosshairModule::Style::CrossX,
        CrosshairModule::Style::Vertical, CrosshairModule::Style::Horizontal,
        CrosshairModule::Style::MilDot,  CrosshairModule::Style::Brackets,
        CrosshairModule::Style::BrokenRing, CrosshairModule::Style::Grid,
        CrosshairModule::Style::RingTicks, CrosshairModule::Style::Converge,
    };
    bool centerClear = true;
    for (const auto style : kGapped) {
        for (const auto& c : shapeFor(style)) {
            if (distanceToCenter(c) < 2.5f) centerClear = false;
        }
    }
    check(centerClear, "gapped styles keep the aim point free of geometry");

    {
        const auto cmds = shapeFor(CrosshairModule::Style::Vertical);
        bool vertical = cmds.size() == 2;
        for (const auto& c : cmds) {
            if (c.type != PL_DRAW_LINE || std::fabs(c.x) > 0.01f || std::fabs(c.w) > 0.01f) vertical = false;
        }
        const float top = std::min(cmds[0].y, cmds[0].y + cmds[0].h);
        const float bottom = std::max(cmds[1].y, cmds[1].y + cmds[1].h);
        check(vertical && top < 0.0f && bottom > 0.0f && std::fabs(top + bottom) < 0.01f,
              "Vertical draws two symmetric arms on the y axis");

        const auto horiz = shapeFor(CrosshairModule::Style::Horizontal);
        bool horizontal = horiz.size() == 2;
        for (const auto& c : horiz) {
            if (c.type != PL_DRAW_LINE || std::fabs(c.y) > 0.01f || std::fabs(c.h) > 0.01f) horizontal = false;
        }
        const float left = std::min(horiz[0].x, horiz[0].x + horiz[0].w);
        const float right = std::max(horiz[1].x, horiz[1].x + horiz[1].w);
        check(horizontal && left < 0.0f && right > 0.0f && std::fabs(left + right) < 0.01f,
              "Horizontal draws two symmetric arms on the x axis");
    }

    {
        // Brackets/BracketsDot: every arm starts on a corner of the box, which
        // is what makes the frame read as four separate corners.
        const float half = 9.0f * 1.1f;
        const auto cmds = shapeFor(CrosshairModule::Style::Brackets);
        bool onCorners = cmds.size() == 8;
        for (const auto& c : cmds) {
            if (std::fabs(std::fabs(c.x) - half) > 0.01f ||
                std::fabs(std::fabs(c.y) - half) > 0.01f) {
                onCorners = false;
            }
        }
        check(onCorners, "Brackets anchors every arm on a box corner");
    }

    {
        const auto cmds = shapeFor(CrosshairModule::Style::Target);
        float nearest = 1e9f, farthest = 0.0f;
        int lines = 0, dots = 0;
        for (const auto& c : cmds) {
            if (c.type == PL_DRAW_LINE) {
                ++lines;
                const float d = distanceToCenter(c);
                if (d < nearest) nearest = d;
                if (d > farthest) farthest = d;
            } else {
                ++dots;
            }
        }
        check(lines == 40 && dots == 1, "Target draws a large and a small ring plus one dot");
        check(nearest > 4.0f && farthest > 9.0f && farthest < 10.0f,
              "Target rings stay concentric around the aim point");
    }

    {
        // The broken ring's gaps sit on the axes, so no point of it may come
        // closer than ~8.5 degrees to the horizontal or vertical line.
        const auto cmds = shapeFor(CrosshairModule::Style::BrokenRing);
        bool gapsOnAxes = cmds.size() == 40;
        for (const auto& c : cmds) {
            const float pts[2][2] = {{c.x, c.y}, {c.x + c.w, c.y + c.h}};
            for (const auto& q : pts) {
                const float ratio = std::min(std::fabs(q[0]), std::fabs(q[1])) /
                                    std::max(std::fabs(q[0]), std::fabs(q[1]));
                if (ratio < 0.15f) gapsOnAxes = false;
            }
        }
        check(gapsOnAxes, "Broken Ring keeps its four gaps centered on the axes");
    }

    {
        const auto cmds = shapeFor(CrosshairModule::Style::MilDot);
        int dots = 0;
        bool dotsOnArms = true;
        for (const auto& c : cmds) {
            if (c.type != PL_DRAW_RECT_FILLED) continue;
            ++dots;
            const float cx = c.x + c.w * 0.5f;
            const float cy = c.y + c.h * 0.5f;
            if (std::fabs(cx) > 0.01f && std::fabs(cy) > 0.01f) dotsOnArms = false;
        }
        check(dots == 8, "Mil Dots draws eight aiming dots");
        check(dotsOnArms, "Mil Dots keeps every dot on one of the four arms");
    }

    {
        const auto cmds = shapeFor(CrosshairModule::Style::Converge);
        float maxX = 0.0f, maxY = 0.0f;
        for (const auto& c : cmds) {
            maxX = std::max({maxX, std::fabs(c.x), std::fabs(c.x + c.w)});
            maxY = std::max({maxY, std::fabs(c.y), std::fabs(c.y + c.h)});
        }
        check(cmds.size() == 8 && std::fabs(maxX - maxY) < 0.01f,
              "Converge is symmetric on both axes");
    }

    {
        // Square Plus Dot (square, plus and dot): the first eight lines are
        // the Brackets frame anchored on the box corners, the next four are
        // the gapped Cross arms on the axes, and the last primitive is the
        // square center dot. The arms reach past the frame, so the plus
        // sticks out of the square on all four sides.
        const auto cmds = shapeFor(CrosshairModule::Style::SquarePlusDot);
        const float half = 9.0f * 1.1f;   // bracket half size, as in Brackets
        const float gap = 3.0f;           // cross gap, as in Cross
        const float tip = gap + 11.0f;    // cross arm tip, as in Cross
        bool brackets = cmds.size() == 13;
        for (size_t i = 0; i < 8 && i < cmds.size(); ++i) {
            if (cmds[i].type != PL_DRAW_LINE ||
                std::fabs(std::fabs(cmds[i].x) - half) > 0.01f ||
                std::fabs(std::fabs(cmds[i].y) - half) > 0.01f) {
                brackets = false;
            }
        }
        check(brackets, "Square Plus Dot opens with the eight bracket arms on the box corners");

        bool arms = cmds.size() == 13;
        for (size_t i = 8; i < 12 && i < cmds.size(); ++i) {
            const auto& c = cmds[i];
            if (c.type != PL_DRAW_LINE) arms = false;
            const bool vertical = std::fabs(c.x) < 0.01f && std::fabs(c.w) < 0.01f;
            const bool horizontal = std::fabs(c.y) < 0.01f && std::fabs(c.h) < 0.01f;
            if (!vertical && !horizontal) arms = false;
            const float d0 = std::sqrt(c.x * c.x + c.y * c.y);
            const float d1 = std::sqrt((c.x + c.w) * (c.x + c.w) + (c.y + c.h) * (c.y + c.h));
            const float reach = std::max(d0, d1);
            const float inner = std::min(d0, d1);
            if (std::fabs(reach - tip) > 0.01f || std::fabs(inner - gap) > 0.01f) arms = false;
        }
        check(arms, "Square Plus Dot draws four gapped arms on the axes reaching past the frame");

        bool dot = cmds.size() == 13 && cmds.back().type == PL_DRAW_RECT_FILLED &&
                   std::fabs(cmds.back().x + 2.2f) < 0.01f &&
                   std::fabs(cmds.back().y + 2.2f) < 0.01f &&
                   std::fabs(cmds.back().w - 4.4f) < 0.01f &&
                   std::fabs(cmds.back().h - 4.4f) < 0.01f;
        check(dot, "Square Plus Dot ends with the square center dot");

        float maxX = 0.0f, maxY = 0.0f;
        for (const auto& c : cmds) {
            maxX = std::max({maxX, std::fabs(c.x), std::fabs(c.x + c.w)});
            maxY = std::max({maxY, std::fabs(c.y), std::fabs(c.y + c.h)});
        }
        check(cmds.size() == 13 && std::fabs(maxX - tip) < 0.01f && std::fabs(maxY - tip) < 0.01f,
              "Square Plus Dot is symmetric and bounded by the arm tips");
    }

    // Size scales every shape linearly, so a new style cannot drift off center
    // or grow non-uniformly at the extremes of the slider.
    bool linear = true;
    for (int s = 1; s < (int)CrosshairModule::Style::Count; ++s) {
        const auto one = shapeFor((CrosshairModule::Style)s, 1.0f);
        const auto two = shapeFor((CrosshairModule::Style)s, 2.0f);
        if (one.size() != two.size()) {
            linear = false;
            continue;
        }
        for (size_t i = 0; i < one.size(); ++i) {
            if (std::fabs(two[i].x - 2.0f * one[i].x) > 0.01f ||
                std::fabs(two[i].y - 2.0f * one[i].y) > 0.01f ||
                std::fabs(two[i].w - 2.0f * one[i].w) > 0.01f ||
                std::fabs(two[i].h - 2.0f * one[i].h) > 0.01f) {
                linear = false;
            }
        }
    }
    check(linear, "every shape scales linearly with the Size option");

    // The menu radio is generated from the style table: one label per style,
    // in enum order, with the legacy indices untouched so configs written
    // before the new shapes existed keep resolving to the same crosshair.
    auto splitRadio = [](const std::string& text) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t comma = text.find(',', start);
            if (comma == std::string::npos) {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, comma - start));
            start = comma + 1;
        }
        return parts;
    };
    nlohmann::json jRadio;
    mod.saveConfig(jRadio);
    const auto radio = splitRadio(jRadio["m_style"].get<std::string>());
    check(radio.size() == (size_t)((int)CrosshairModule::Style::Count + 1),
          "radio value carries one label per style");
    bool labelsFollowEnum = radio.size() == (size_t)((int)CrosshairModule::Style::Count + 1);
    if (labelsFollowEnum) {
        for (int i = 0; i < (int)CrosshairModule::Style::Count; ++i) {
            if (radio[(size_t)i + 1] != kStyleNames[i]) labelsFollowEnum = false;
        }
    }
    check(labelsFollowEnum, "radio labels follow the style enum");
    check(radio.size() > 17 && radio[1] == "Vanilla" && radio[2] == "Cross" && radio[17] == "Scope",
          "legacy style indices keep their labels (Vanilla 0 ... Scope 16)");
    std::vector<std::string> uniqueLabels(radio.begin() + 1, radio.end());
    std::sort(uniqueLabels.begin(), uniqueLabels.end());
    check(std::unique(uniqueLabels.begin(), uniqueLabels.end()) == uniqueLabels.end(),
          "style labels are unique");

    CrosshairModule probe;
    bool styleRoundTrip = true;
    for (int i = 0; i < (int)CrosshairModule::Style::Count; ++i) {
        mod.m_style = static_cast<CrosshairModule::Style>(i);
        nlohmann::json jStyle;
        mod.saveConfig(jStyle);
        probe.m_style = CrosshairModule::Style::Count;
        probe.loadConfig(jStyle);
        if (probe.m_style != static_cast<CrosshairModule::Style>(i)) styleRoundTrip = false;
    }
    check(styleRoundTrip, "every style index round-trips through the radio value");
    mod.m_style = CrosshairModule::Style::Vanilla;

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
    j3["m_style"] = "25,Broken Ring,Triangle";
    mod3.loadConfig(j3);
    check(mod3.m_style == CrosshairModule::Style::BrokenRing, "new style indices parse");
    const auto styleBeforeBogus = mod3.m_style;
    j3["m_style"] = "99,Bogus";
    mod3.loadConfig(j3);
    check(mod3.m_style == styleBeforeBogus, "out-of-range index ignored");

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

    // Hit indicator: lives entirely in the Crosshair module. Lighting it
    // must recolor the custom overlay without any Hitbox module present.
    g_lastCursorRenderUs.store(0, std::memory_order_relaxed);
    g_aimedEntityInRange.store(false, std::memory_order_relaxed);
    g_aimRefreshTimeUs.store(0, std::memory_order_relaxed);
    g_cursorTintState.store(static_cast<uint32_t>(CursorTintState::Probing),
                            std::memory_order_relaxed);
    g_cursorTintProbeMisses.store(0, std::memory_order_relaxed);
    mod3.m_style = CrosshairModule::Style::Cross;
    mod3.m_rgb = false;
    mod3.m_color = 0xFFFFFFFF;
    mod3.m_indicator = true;
    mod3.m_indicatorColor = 0xFFFF3300;
    mod3.m_outline = false;
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod3.onFrame();
    check(!g_lastCmds.empty(), "custom style still draws with indicator idle");
    check((g_lastCmds.back().color & 0x00FFFFFF) == 0xFFFFFF, "idle indicator keeps configured color");

    g_aimedEntityInRange.store(true, std::memory_order_relaxed);
    g_aimRefreshTimeUs.store(nowUs(), std::memory_order_relaxed);
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod3.onFrame();
    check(!g_lastCmds.empty(), "indicator draws without Hitbox module");
    check((g_lastCmds.back().color & 0x00FFFFFF) == 0xFF3300, "aimed indicator uses indicator color");
    check(indicatorLit(), "indicatorLit is true while the aim flag is fresh");

    g_aimedEntityInRange.store(false, std::memory_order_relaxed);
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod3.onFrame();
    check((g_lastCmds.back().color & 0x00FFFFFF) == 0xFFFFFF, "looking away restores configured color");

    // Config round-trip for the indicator settings.
    nlohmann::json jInd;
    mod3.saveConfig(jInd);
    check(jInd["m_indicator"].get<bool>(), "indicator persists as true");
    CrosshairModule mod4;
    mod4.loadConfig(jInd);
    check(mod4.m_indicator, "indicator flag round-trip");
    check((mod4.m_indicatorColor & 0x00FFFFFF) == 0xFF3300, "indicator color round-trip");

    // Vanilla style + indicator: after several tint-probe misses the overlay
    // fallback must hide the vanilla draw and submit a replacement.
    g_originalCursorCalls = 0;
    g_lastCursorRenderUs.store(0, std::memory_order_relaxed);
    g_aimedEntityInRange.store(true, std::memory_order_relaxed);
    g_aimRefreshTimeUs.store(nowUs(), std::memory_order_relaxed);
    g_cursorTintState.store(static_cast<uint32_t>(CursorTintState::Probing),
                            std::memory_order_relaxed);
    g_cursorTintProbeMisses.store(0, std::memory_order_relaxed);
    // mod4 is now g_crosshairMod; enable it and copy the indicator settings.
    mod4.setMasterEnabled(true);
    mod4.m_style = CrosshairModule::Style::Vanilla;
    mod4.m_indicator = true;
    mod4.m_indicatorColor = 0xFFFF3300;
    mod4.m_outline = false;
    for (int i = 0; i < 8; ++i) {
        cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    }
    check(g_originalCursorCalls == 8, "vanilla indicator probes the original renderer");
    check(overlayFallbackLatched(), "vanilla indicator latches overlay fallback");
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    check(g_originalCursorCalls == 8, "fallback swallows the vanilla renderer");
    mod4.onFrame();
    check(g_lastCmds.size() >= 4, "vanilla indicator fallback draws replacement arms");
    check((g_lastCmds.back().color & 0x00FFFFFF) == 0xFF3300, "fallback arms use indicator color");

    // Show In Third Person: the default keeps the crosshair first-person
    // only, like vanilla. Enabling the option should render the overlay in
    // third person too.
    g_lastCursorRenderUs.store(0, std::memory_order_relaxed);
    g_aimedEntityInRange.store(false, std::memory_order_relaxed);
    g_aimRefreshTimeUs.store(0, std::memory_order_relaxed);
    g_perspectiveKnown.store(true, std::memory_order_relaxed);
    g_isThirdPerson.store(true, std::memory_order_relaxed);
    mod4.m_style = CrosshairModule::Style::Cross;
    mod4.m_indicator = false;
    mod4.m_showThirdPerson = false;
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod4.onFrame();
    check(g_lastCmds.empty(), "third person draw is hidden while the option is off");

    mod4.m_showThirdPerson = true;
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod4.onFrame();
    check(!g_lastCmds.empty(), "third person draw appears once the option is on");

    g_isThirdPerson.store(false, std::memory_order_relaxed);
    cursorRenderHook(nullptr, nullptr, nullptr, nullptr);
    mod4.onFrame();
    check(!g_lastCmds.empty(), "first person draw still works after toggling back");

    // Third-person flag round-trip through config.
    nlohmann::json jThird;
    mod4.saveConfig(jThird);
    check(jThird["m_showThirdPerson"].get<bool>(), "show-third-person persists as true");
    CrosshairModule mod5;
    mod5.loadConfig(jThird);
    check(mod5.m_showThirdPerson, "show-third-person flag round-trip");

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
