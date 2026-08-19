#include "crosshair.hpp"

#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using HudCursorRenderFn = void (*)(void*, void*, void*, void*);

CrosshairModule* g_crosshairMod = nullptr;
HudCursorRenderFn g_cursorRenderOrig = nullptr;

// Timestamp of the last HudCursorRenderer::render call that the module
// swallowed. Read by onFrame to know whether the HUD is actually showing a
// crosshair right now.
std::atomic<int64_t> g_lastCursorRenderUs{0};

int64_t nowUs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

double nowSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// HudCursorRenderer::render is the function that draws the vanilla
// crosshair (textures/gui/crosshair.png). The debug menu and the hitbox
// crosshair indicator hook the same target; pl::memory::hook chains the
// detours, so as long as every module keeps forwarding to the original all
// of them keep working together.
//
// While a custom style is selected the vanilla draw is swallowed here and
// onFrame submits the replacement shape through the HUD overlay instead, so
// there is still exactly one crosshair on screen - never one painted over
// the other. With Style::Vanilla the call is forwarded untouched and the
// module behaves as if it were disabled.
void cursorRenderHook(void* _this, void* a1, void* a2, void* a3) {
    if (!g_cursorRenderOrig) return;

    if (g_crosshairMod && g_crosshairMod->enabled && g_crosshairMod->customStyleActive()) {
        g_lastCursorRenderUs.store(nowUs(), std::memory_order_relaxed);
        return;
    }

    g_cursorRenderOrig(_this, a1, a2, a3);
}

// True when the cursor renderer ran very recently, i.e. the game itself
// would be showing its crosshair right now (first person, no menu open).
// Guards the overlay so a custom crosshair is never painted where the game
// would not draw one (third person, pause/menu screens, touch layouts
// without a crosshair).
bool cursorRenderRecent() {
    constexpr int64_t kMaxAgeUs = 100000; // 100 ms, ~6 frames at 60 fps
    const int64_t last = g_lastCursorRenderUs.load(std::memory_order_relaxed);
    return last > 0 && (nowUs() - last) <= kMaxAgeUs;
}

// s = v = 1 rainbow color for the RGB mode. hue in degrees [0, 360).
uint32_t hsvToRgb(float hue) {
    const float x = 1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hue < 60.0f)       { r = 1.0f; g = x; }
    else if (hue < 120.0f) { r = x;    g = 1.0f; }
    else if (hue < 180.0f) { g = 1.0f; b = x; }
    else if (hue < 240.0f) { g = x;    b = 1.0f; }
    else if (hue < 300.0f) { r = x;    b = 1.0f; }
    else                   { r = 1.0f; b = x; }
    return (static_cast<uint32_t>(r * 255.0f) << 16) |
           (static_cast<uint32_t>(g * 255.0f) << 8) |
            static_cast<uint32_t>(b * 255.0f);
}

constexpr float kPi = 3.14159265f;

// Small helper that appends primitives relative to the crosshair center.
// One shape build runs once per pass (outline pass + color pass).
struct ShapePainter {
    std::vector<PLModMenu_DrawCommand>& cmds;
    float cx;
    float cy;
    float thickness;
    uint32_t color;

    // Line from center + (x1, y1) to center + (x2, y2).
    void line(float x1, float y1, float x2, float y2) const {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_LINE;
        cmd.x = cx + x1;
        cmd.y = cy + y1;
        cmd.w = x2 - x1; // launcher treats w/h as the end-point delta
        cmd.h = y2 - y1;
        cmd.size = thickness;
        cmd.color = color;
        cmds.push_back(cmd);
    }

    // Axis-aligned rect; x/y relative to the center is the top-left corner.
    void rect(float x, float y, float w, float h) const {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_RECT_FILLED;
        cmd.x = cx + x;
        cmd.y = cy + y;
        cmd.w = w;
        cmd.h = h;
        cmd.color = color;
        cmds.push_back(cmd);
    }

    // Square center dot with the given half size.
    void dot(float halfSize) const {
        rect(-halfSize, -halfSize, halfSize * 2.0f, halfSize * 2.0f);
    }

    // Hollow circle approximated with line segments, so only the well-known
    // line primitive is needed. 24 segments stay smooth up to large scales.
    void circle(float radius, int segments = 24) const {
        const float step = 2.0f * kPi / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i) {
            const float a0 = static_cast<float>(i) * step;
            const float a1 = static_cast<float>(i + 1) * step;
            line(radius * std::cos(a0), radius * std::sin(a0),
                 radius * std::cos(a1), radius * std::sin(a1));
        }
    }
};

void buildShape(CrosshairModule::Style style, const ShapePainter& p, float s) {
    const float arm = 11.0f * s;  // arm length for cross-like shapes
    const float gap = 3.0f * s;   // empty space around the exact center
    const float radius = 9.0f * s;  // hollow-shape radius
    const float halfDot = 2.2f * s; // center dot half size

    auto cross = [&](float from, float to) {
        p.line(0.0f, -from, 0.0f, -to);  // top
        p.line(0.0f, from, 0.0f, to);    // bottom
        p.line(-from, 0.0f, -to, 0.0f);  // left
        p.line(from, 0.0f, to, 0.0f);    // right
    };

    switch (style) {
        case CrosshairModule::Style::Cross:
            cross(gap, gap + arm);
            break;

        case CrosshairModule::Style::Dot:
            p.dot(halfDot);
            break;

        case CrosshairModule::Style::CrossDot:
            cross(gap, gap + arm);
            p.dot(halfDot);
            break;

        case CrosshairModule::Style::Circle:
            p.circle(radius);
            break;

        case CrosshairModule::Style::CircleDot:
            p.circle(radius);
            p.dot(halfDot);
            break;

        case CrosshairModule::Style::CircleCross:
            p.circle(radius);
            cross(gap, radius * 0.6f);
            break;

        case CrosshairModule::Style::Square: {
            const float h = radius * 0.8f; // half side
            p.line(-h, -h, h, -h);
            p.line(h, -h, h, h);
            p.line(h, h, -h, h);
            p.line(-h, h, -h, -h);
            break;
        }

        case CrosshairModule::Style::SquareDot: {
            const float h = radius * 0.8f;
            p.line(-h, -h, h, -h);
            p.line(h, -h, h, h);
            p.line(h, h, -h, h);
            p.line(-h, h, -h, -h);
            p.dot(halfDot);
            break;
        }

        case CrosshairModule::Style::Diamond:
            p.line(0.0f, -radius, radius, 0.0f);
            p.line(radius, 0.0f, 0.0f, radius);
            p.line(0.0f, radius, -radius, 0.0f);
            p.line(-radius, 0.0f, 0.0f, -radius);
            break;

        case CrosshairModule::Style::Plus:
            p.line(-arm, 0.0f, arm, 0.0f);
            p.line(0.0f, -arm, 0.0f, arm);
            break;

        case CrosshairModule::Style::X: {
            const float d = radius * 0.9f;
            p.line(-d, -d, d, d);
            p.line(-d, d, d, -d);
            break;
        }

        case CrosshairModule::Style::TShape: {
            const float h = arm * 0.65f; // half bar width
            p.line(0.0f, 0.0f, 0.0f, -arm);   // stem up from the center
            p.line(-h, -arm, h, -arm);        // top bar
            break;
        }

        case CrosshairModule::Style::Chevron: {
            const float w = radius * 0.75f;
            const float top = -radius * 0.75f;
            const float bottom = radius * 0.5f;
            p.line(-w, bottom, 0.0f, top);
            p.line(0.0f, top, w, bottom);
            break;
        }

        case CrosshairModule::Style::Arrow: {
            const float tip = -arm;
            const float w = radius * 0.55f;
            p.line(0.0f, arm * 0.7f, 0.0f, tip);          // shaft
            p.line(0.0f, tip, -w, tip + radius * 0.7f);   // head left
            p.line(0.0f, tip, w, tip + radius * 0.7f);    // head right
            break;
        }

        case CrosshairModule::Style::Star: {
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * (kPi / 4.0f);
                const float len = (i % 2 == 0) ? arm : arm * 0.7f;
                p.line(0.0f, 0.0f, len * std::cos(a), len * std::sin(a));
            }
            break;
        }

        case CrosshairModule::Style::Scope: {
            const float r = radius * 1.4f;
            p.circle(r);
            cross(gap, arm * 1.45f);
            break;
        }

        case CrosshairModule::Style::Vanilla:
        case CrosshairModule::Style::Count:
        default:
            break;
    }
}

} // namespace

CrosshairModule::CrosshairModule()
    : Module("Crosshair", "Replaces the vanilla crosshair with 16 custom shapes. Color, size, thickness, outline and an animated RGB mode are configurable.") {
    // The crosshair always sits at the exact screen center; there is nothing
    // to drag in the HUD editor.
    hideInHudEditor = true;
    g_crosshairMod = this;
}

CrosshairModule::~CrosshairModule() {
    if (g_crosshairMod == this) g_crosshairMod = nullptr;
}

void CrosshairModule::onInit() {
    // Installed once for the whole session (the detour passes straight
    // through whenever the module is off or the Vanilla style is selected,
    // and it chains safely with the debug menu/hitbox cursor hooks).
    if (m_cursorHooked) return;
    uintptr_t cursor = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HudCursor);
    if (cursor != 0 &&
        bedrocktools::hooks::install(reinterpret_cast<void*>(cursor),
                                     reinterpret_cast<void*>(&cursorRenderHook),
                                     reinterpret_cast<void**>(&g_cursorRenderOrig))) {
        m_cursorHooked = true;
    }
}

void CrosshairModule::onEnable() {
}

void CrosshairModule::onDisable() {
    g_lastCursorRenderUs.store(0, std::memory_order_relaxed);
    // Clear the overlay immediately; onFrame stops running for disabled
    // modules, so this is the only chance to remove the custom crosshair.
    submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
}

void CrosshairModule::onFrame() {
    if (!enabled || !customStyleActive() || !cursorRenderRecent()) {
        submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    // Resolve the draw color: animated RGB or the configured static color.
    uint32_t rgb;
    if (m_rgb) {
        const float speed = std::clamp(m_rgbSpeed, 0.05f, 1.0f);
        float hue = std::fmod(static_cast<float>(nowSeconds()) * speed * 360.0f, 360.0f);
        if (hue < 0.0f) hue += 360.0f;
        rgb = hsvToRgb(hue);
    } else {
        rgb = m_color & 0x00FFFFFFu;
    }

    const uint8_t alpha = static_cast<uint8_t>(std::clamp(m_opacity, 0.0f, 1.0f) * 255.0f);
    const uint32_t color = (static_cast<uint32_t>(alpha) << 24) | rgb;
    const uint32_t outline = static_cast<uint32_t>(static_cast<float>(alpha) * 0.78f) << 24;

    const float scale = std::clamp(m_scale, 0.1f, 5.0f);
    const float thickness = std::clamp(m_thickness, 0.5f, 20.0f);

    // HUD overlay coordinates: values <= -19000 are interpreted by the
    // launcher relative to the screen center, with -20000 being the exact
    // center (the same convention the debug menu and hitbox overlay use), so
    // the crosshair is always dead-center on every resolution.
    constexpr float kCenter = -20000.0f;

    std::vector<PLModMenu_DrawCommand> cmds;
    if (m_outline) {
        // Dark, slightly thicker back-pass first so the crosshair stays
        // readable against bright skies and sand.
        ShapePainter outlinePainter{cmds, kCenter, kCenter, thickness + 2.0f, outline};
        buildShape(m_style, outlinePainter, scale);
    }
    ShapePainter painter{cmds, kCenter, kCenter, thickness, color};
    buildShape(m_style, painter, scale);

    submitDrawCommands(moduleId, cmds);
}

void CrosshairModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("m_style")) {
        const auto& value = j["m_style"];
        // Radio configs persist as "<index>,<label>..." strings; the menu
        // itself reports a plain index when the selection changes.
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const auto comma = text.find(',');
            try {
                const int style = std::stoi(text.substr(0, comma));
                if (style >= 0 && style < static_cast<int>(Style::Count)) {
                    m_style = static_cast<Style>(style);
                }
            } catch (...) {}
        } else if (value.is_number_integer()) {
            const int style = value.get<int>();
            if (style >= 0 && style < static_cast<int>(Style::Count)) {
                m_style = static_cast<Style>(style);
            }
        }
    }

    if (j.contains("m_scale")) {
        try { m_scale = std::clamp(j["m_scale"].get<float>(), 0.1f, 5.0f); } catch (...) {}
    }
    if (j.contains("m_thickness")) {
        try { m_thickness = std::clamp(j["m_thickness"].get<float>(), 0.5f, 20.0f); } catch (...) {}
    }
    if (j.contains("m_opacity")) {
        try { m_opacity = std::clamp(j["m_opacity"].get<float>(), 0.0f, 1.0f); } catch (...) {}
    }

    if (j.contains("m_color") && j["m_color"].is_string()) {
        std::string hexStr = j["m_color"].get<std::string>();
        if (!hexStr.empty()) {
            if (hexStr[0] == '#') hexStr = hexStr.substr(1);
            else if (hexStr.size() > 1 && hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')) hexStr = hexStr.substr(2);
            try {
                const unsigned long parsed = std::stoul(hexStr, nullptr, 16);
                // Accept both #RRGGBB (menu color picker) and #AARRGGBB;
                // either way only the RGB channels are kept.
                m_color = 0xFF000000u | (static_cast<uint32_t>(parsed) & 0x00FFFFFFu);
            } catch (...) {}
        }
    }

    if (j.contains("m_rgb")) {
        try { m_rgb = j["m_rgb"].get<bool>(); } catch (...) {}
    }
    if (j.contains("m_rgbSpeed")) {
        try { m_rgbSpeed = std::clamp(j["m_rgbSpeed"].get<float>(), 0.05f, 1.0f); } catch (...) {}
    }
    if (j.contains("m_outline")) {
        try { m_outline = j["m_outline"].get<bool>(); } catch (...) {}
    }
    if (j.contains("m_indicator")) {
        try { m_indicator = j["m_indicator"].get<bool>(); } catch (...) {}
    }
    if (j.contains("m_indicatorColor") && j["m_indicatorColor"].is_string()) {
        std::string hexStr = j["m_indicatorColor"].get<std::string>();
        if (!hexStr.empty()) {
            if (hexStr[0] == '#') hexStr = hexStr.substr(1);
            try { m_indicatorColor = 0xFF000000u | (static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16)) & 0x00FFFFFFu); } catch (...) {}
        }
    }
}

void CrosshairModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_style"] = std::to_string(static_cast<int>(m_style)) +
        ",Vanilla,Cross,Dot,Cross Dot,Circle,Circle Dot,Circle Cross,"
        "Square,Square Dot,Diamond,Plus,X,T Shape,Chevron,Arrow,Star,Scope";
    j["m_scale"] = m_scale;
    j["m_thickness"] = m_thickness;
    j["m_opacity"] = m_opacity;

    char color[10];
    std::snprintf(color, sizeof(color), "#%06X", m_color & 0x00FFFFFFu);
    j["m_color"] = std::string(color);

    j["m_rgb"] = m_rgb;
    j["m_rgbSpeed"] = m_rgbSpeed;
    j["m_outline"] = m_outline;
    j["m_indicator"] = m_indicator;

    char indicatorColor[10];
    std::snprintf(indicatorColor, sizeof(indicatorColor), "#%06X", m_indicatorColor & 0x00FFFFFFu);
    j["m_indicatorColor"] = std::string(indicatorColor);
}
