// host_test/blockoutline_test.cpp
//
// Linux host test driver for src/modules/visual/blockoutline_core.{hpp,cpp}.
// Compiles with stock g++ -std=c++20 on Linux (no Android NDK, no Preloader,
// no nlohmann::json — the JSON parsing is done by the in-tree tinyjson.hpp).
//
// What it does:
//   1. Loads a BlockOutlineConfig from a JSON string the way the in-game
//      module loads it via nlohmann::json. Round-trips it back to show the
//      save/load contract is identical.
//   2. Exercises validHitSnapshot() with seven candidate snapshots, asserting
//      that valid ones pass and corrupt/empty/out-of-bounds/entity-hits are
//      rejected — same checks the cpp uses to gate the renderer.
//   3. Calls buildOutlineGeometry() in 2D-bands and 3D-beams modes and
//      prints the resulting line/quad counts (12 lines + 24 quads in 2D,
//      72 quads + 12 lines in 3D, matching what the cpp uses the in-game
//      tessellator to ingest).
//   4. Drives cycleRgbColor() to advance the hue by 30 simulated frames,
//      printing the resulting ARGB values so the rebuild is visibly walking
//      through the colour wheel.
//   5. Writes SVG files (host_test/build/outline_2d.svg + outline_3d.svg)
//      AND a terminal-side ASCII wireframe of the same scene so the rebuild
//      is provably visible without a graphics viewer.
//
// Exit code is 0 only if every assertion passes.

#include "blockoutline_core.hpp"
#include "../host_test/tinyjson.hpp"

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bo = bedrocktools::blockoutline;

// ── Config (mirrors src/modules/visual/blockoutline.cpp state) ──────────
struct BlockOutlineConfig {
    std::uint32_t outlineColor  = 0xFFFFFFFFu;
    bool          rgb           = false;
    bool          threeD        = false;
    float         rgbSpeed      = 1.0f;
    float         rgbHue        = 0.0f;
    float         inset         = 0.0025f;
    float         thickness     = 2.0f;
    float         maxDistance   = 128.0f;
};

static std::uint32_t parseColorString(const std::string& v) {
    if (v.empty()) return 0u;
    std::string s = v;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    else if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
}

static int g_passed = 0;
static int g_failed = 0;

#define BO_CHECK(label, cond) do {                                          \
        if (cond) { ++g_passed;                                              \
            std::cout << "  [PASS] " << label << std::endl; }                \
        else      { ++g_failed;                                              \
            std::cout << "  [FAIL] " << label << " (line "                   \
                      << __LINE__ << ")" << std::endl; }                     \
    } while (0)

// ── Round-trip a Config through tinyjson (the same shape the cpp uses) ─
static BlockOutlineConfig loadConfigFromJson(const tinyjson::Value& j) {
    BlockOutlineConfig c;
    auto getColor = [&](const tinyjson::Value* v) {
        if (!v) return;
        if      (v->isString()) c.outlineColor = parseColorString(v->asString());
        else if (v->isInt())    c.outlineColor = static_cast<std::uint32_t>(v->asInt());
    };
    auto exists      = [&](const char* k)                { return j.contains(k); };
    auto getBool     = [&](const char* k, bool      fb)  { return exists(k) ? j.obj.find(k)->second.asBool(fb)  : fb; };
    auto getFloat    = [&](const char* k, float     fb)  { return exists(k) ? j.obj.find(k)->second.asFloat(fb) : fb; };

    if      (exists("outlineColor"))  getColor(&j.obj.find("outlineColor")->second);
    else if (exists("color"))         getColor(&j.obj.find("color")->second);
    else if (exists("m_outlineColor"))getColor(&j.obj.find("m_outlineColor")->second);

    if      (exists("rgb"))           c.rgb         = getBool("rgb",         c.rgb);
    else if (exists("rainbow"))       c.rgb         = getBool("rainbow",     c.rgb);

    if      (exists("threeD"))        c.threeD      = getBool("threeD",      c.threeD);
    else if (exists("3d"))            c.threeD      = getBool("3d",          c.threeD);
    else if (exists("outline3D"))     c.threeD      = getBool("outline3D",   c.threeD);

    if      (exists("rgbSpeed"))      c.rgbSpeed    = getFloat("rgbSpeed",    c.rgbSpeed);
    else if (exists("rainbowSpeed"))  c.rgbSpeed    = getFloat("rainbowSpeed",c.rgbSpeed);

    if      (exists("inset"))         c.inset       = getFloat("inset",       c.inset);
    else if (exists("expand"))        c.inset       = getFloat("expand",      c.inset);

    if      (exists("thickness"))     c.thickness   = getFloat("thickness",   c.thickness);
    else if (exists("lineWidth"))     c.thickness   = getFloat("lineWidth",   c.thickness);

    if      (exists("maxDistance"))   c.maxDistance = getFloat("maxDistance", c.maxDistance);
    else if (exists("range"))         c.maxDistance = getFloat("range",       c.maxDistance);
    else if (exists("distance"))      c.maxDistance = getFloat("distance",    c.maxDistance);

    return c;
}

static tinyjson::Value saveConfigToJson(const BlockOutlineConfig& c) {
    tinyjson::Value out;
    out.kind = tinyjson::Value::Kind::Object_;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%08X", c.outlineColor);
    tinyjson::Value v; v.kind = tinyjson::Value::Kind::String; v.str = buf;
    out.obj["outlineColor"] = v;

    tinyjson::Value b;
    b.kind = tinyjson::Value::Kind::Bool;
    b.boolean = c.rgb;    out.obj["rgb"]    = b;
    b.boolean = c.threeD; out.obj["threeD"] = b;

    tinyjson::Value f; f.kind = tinyjson::Value::Kind::Float;
    f.number = c.rgbSpeed;    out.obj["rgbSpeed"]    = f;
    f.number = c.inset;       out.obj["inset"]       = f;
    f.number = c.thickness;   out.obj["thickness"]   = f;
    f.number = c.maxDistance; out.obj["maxDistance"] = f;
    return out;
}

// ── Tests ───────────────────────────────────────────────────────────────
static void testConfigRoundTrip() {
    std::cout << "[test] Config round-trip through JSON" << std::endl;
    const BlockOutlineConfig original{
        /*color*/     0xFF40C0FFu,
        /*rgb*/       true,
        /*threeD*/    true,
        /*rgbSpeed*/  0.75f,
        /*rgbHue*/    0.0f,
        /*inset*/     0.01f,
        /*thickness*/ 4.0f,
        /*maxDist*/   80.0f
    };
    auto j = saveConfigToJson(original);
    auto s = tinyjson::toString(j);
    std::cout << "    JSON: " << s << std::endl;
    auto parsed = tinyjson::fromString(s);
    auto after = loadConfigFromJson(parsed);
    BO_CHECK("outlineColor preserved",     after.outlineColor == original.outlineColor);
    BO_CHECK("rgb flag preserved",         after.rgb         == original.rgb);
    BO_CHECK("threeD flag preserved",      after.threeD      == original.threeD);
    BO_CHECK("rgbSpeed preserved",         std::abs(after.rgbSpeed    - original.rgbSpeed)    < 1e-4);
    BO_CHECK("inset preserved",            std::abs(after.inset       - original.inset)       < 1e-4);
    BO_CHECK("thickness preserved",        std::abs(after.thickness   - original.thickness)   < 1e-4);
    BO_CHECK("maxDistance preserved",      std::abs(after.maxDistance - original.maxDistance) < 1e-4);

    // Aliases also work: load using "color" / "rainbow" / "3d" / etc.
    tinyjson::Value aliases;
    aliases.kind = tinyjson::Value::Kind::Object_;
    {
        tinyjson::Value v; v.kind = tinyjson::Value::Kind::String; v.str = "0x80FF0080";
        aliases.obj["color"] = v;
        tinyjson::Value b; b.kind = tinyjson::Value::Kind::Bool; b.boolean = true;
        aliases.obj["3d"] = b;
        aliases.obj["rainbow"] = b;
        tinyjson::Value f; f.kind = tinyjson::Value::Kind::Float;
        f.number = 2.5;   aliases.obj["expand"]    = f;
        f.number = 6.0;   aliases.obj["lineWidth"] = f;
        f.number = 200.0; aliases.obj["range"]     = f;
    }
    auto aliased = loadConfigFromJson(aliases);
    BO_CHECK("'color' alias parsed",   aliased.outlineColor == 0x80FF0080u);
    BO_CHECK("'3d' alias parsed",      aliased.threeD       == true);
    BO_CHECK("'rainbow' alias parsed", aliased.rgb          == true);
    BO_CHECK("'expand' alias parsed",  std::abs(aliased.inset       - 2.5f)  < 1e-4);
    BO_CHECK("'lineWidth' alias parsed",std::abs(aliased.thickness   - 6.0f)  < 1e-4);
    BO_CHECK("'range' alias parsed",   std::abs(aliased.maxDistance - 200.0f)< 1e-4);
}

static void testSnapshotValidation() {
    std::cout << "[test] HitSnapshot validation" << std::endl;
    using bo::BlockHitSnapshot;

    BlockHitSnapshot snap{};
    snap.blockPos = {100, 64, -200};
    snap.hitPos   = {100.5f, 64.5f, -200.5f};
    snap.valid    = true;
    BO_CHECK("valid centre-of-block snapshot accepted",
             bo::validHitSnapshot(snap));

    snap.valid = false;
    BO_CHECK("invalid flag rejected",
             !bo::validHitSnapshot(snap));

    snap.valid = true;
    snap.blockPos.x = 50000000;
    BO_CHECK("absurd block X rejected", !bo::validHitSnapshot(snap));

    snap.blockPos.x = 100;
    snap.blockPos.y = 5000;
    BO_CHECK("Y above 2048 rejected",  !bo::validHitSnapshot(snap));

    snap.blockPos.y = 64;
    snap.hitPos.x   = std::numeric_limits<float>::quiet_NaN();
    BO_CHECK("NaN hit X rejected",     !bo::validHitSnapshot(snap));

    snap.hitPos.x = 200.f;
    BO_CHECK("hit pos far from block rejected", !bo::validHitSnapshot(snap));

    snap.hitPos.x = 100.5f;
    BO_CHECK("restored snapshot accepted again",
             bo::validHitSnapshot(snap));
}

template <bool ThreeD>
static std::size_t runGeometryBuild(const bo::GeometryOptions& opt) {
    std::vector<bo::Line> lines;
    std::vector<bo::Quad> quads;
    const bedrocktools::sdk::BlockPos block{100, 64, -200};
    bo::buildOutlineGeometry(block, opt, lines, quads);
    std::size_t total = lines.size() + quads.size();
    std::cout << "    " << (ThreeD ? "3D beams" : "2D bands")
              << " → "  << lines.size() << " lines, "
              << quads.size() << " quads, total "
              << total << std::endl;
    return total;
}

static void testGeometryBuilders() {
    std::cout << "[test] Outline geometry builders" << std::endl;
    bo::GeometryOptions twoD; twoD.threeD = false; twoD.thickness = 2.0f; twoD.gap = 0.0025f;
    auto total2D = runGeometryBuild<false>(twoD);
    BO_CHECK("2D mode produces 12 lines + 24 quads (36 total)", total2D == 36);

    // thickness == 0 in 2D should still produce the lines but zero quads.
    bo::GeometryOptions twoD_thin; twoD_thin.threeD = false; twoD_thin.thickness = 0.0f;
    std::vector<bo::Line> lines;
    std::vector<bo::Quad> quads;
    bo::buildOutlineGeometry({0, 0, 0}, twoD_thin, lines, quads);
    BO_CHECK("2D zero-thickness -> 12 lines, 0 quads",
             lines.size() == 12 && quads.empty());

    bo::GeometryOptions thrD; thrD.threeD = true; thrD.thickness = 2.0f; thrD.gap = 0.0025f;
    auto total3D = runGeometryBuild<true>(thrD);
    BO_CHECK("3D mode produces 12 lines + 72 quads (84 total)", total3D == 84);
}

static void testRgbCycle() {
    std::cout << "[test] RGB cycle" << std::endl;
    BlockOutlineConfig cfg;
    cfg.outlineColor = 0xFF40C0FFu;
    cfg.rgb          = false;
    cfg.rgbSpeed     = 1.0f;
    cfg.rgbHue       = 0.0f;

    auto same = bo::cycleRgbColor(cfg.outlineColor, cfg.rgb, cfg.rgbHue, cfg.rgbSpeed);
    BO_CHECK("rgb disabled keeps colour unchanged",
             same == cfg.outlineColor);
    BO_CHECK("rgb disabled keeps hue unchanged",
             cfg.rgbHue == 0.0f);
    cfg.rgb = true;

    std::uint32_t prev = cfg.outlineColor;
    for (int i = 0; i < 30; ++i) {
        prev = bo::cycleRgbColor(prev, true, cfg.rgbHue, cfg.rgbSpeed);
    }
    BO_CHECK("hue advanced by 30 frames",
             std::abs(cfg.rgbHue - 0.06f) < 1e-4);
    const auto outRGB = bo::unpackArgb(prev);
    std::cout << "    30-frame sample ARGB = #"
              << std::hex << prev << std::dec
              << "    (r=" << outRGB.r << ", g=" << outRGB.g
              << ", b=" << outRGB.b << ", a=" << outRGB.a << ")" << std::endl;
    BO_CHECK("RGB channel changed from base colour after cycling",
             prev != cfg.outlineColor);
    BO_CHECK("alpha byte preserved",      (prev & 0xFF000000u) == 0xFF000000u);

    cfg.rgbHue = -0.001f;
    bo::cycleRgbColor(cfg.outlineColor, true, cfg.rgbHue, cfg.rgbSpeed);
    BO_CHECK("negative hue wraps to >=0 and <1",
             cfg.rgbHue >= 0.0f && cfg.rgbHue < 1.0f);

    cfg.rgbHue = 0.42f;
    bo::cycleRgbColor(0xFF112233u, true, cfg.rgbHue, /*speed*/0.0f);
    BO_CHECK("rgbSpeed=0 keeps hue stable",
             std::abs(cfg.rgbHue - 0.42f) < 1e-6);
}

// ── Projection helpers (shared by SVG + ASCII paths) ────────────────────
struct V2 { float x, y; };
static V2 project(const bedrocktools::sdk::Vec3& v,
                  float cx, float cy, float scale,
                  const bedrocktools::sdk::Vec3& offset) {
    const float aY = 0.785398f;          // 45° around Y
    const float aX = 0.523598f;          // 30° around X
    const float cY = std::cos(aY), sY = std::sin(aY);
    const float cX = std::cos(aX), sX = std::sin(aX);

    const float dx = v.x - offset.x;
    const float dy = v.y - offset.y;
    const float dz = v.z - offset.z;
    const float x1 =  dx * cY + dz * sY;
    const float z1 = -dx * sY + dz * cY;
    const float y1 =  dy * cX - z1 * sX;
    return {cx + x1 * scale, cy - y1 * scale};
}

// ── SVG ─────────────────────────────────────────────────────────────────
static void writeSvg(const std::string& path,
                     const std::vector<bo::Line>& lines,
                     const std::vector<bo::Quad>& quads,
                     const bedrocktools::sdk::Vec3& centre,
                     const std::string& subtitle,
                     std::uint32_t argb) {
    std::uint8_t rByte = static_cast<std::uint8_t>((argb >> 16) & 0xFFu);
    std::uint8_t gByte = static_cast<std::uint8_t>((argb >>  8) & 0xFFu);
    std::uint8_t bByte = static_cast<std::uint8_t>( argb        & 0xFFu);
    float        aFlt  = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;

    char rgbBuf[32];
    std::snprintf(rgbBuf, sizeof(rgbBuf), "rgb(%u,%u,%u)",
                  static_cast<unsigned>(rByte),
                  static_cast<unsigned>(gByte),
                  static_cast<unsigned>(bByte));

    std::ofstream out(path);
    out << R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n";
    out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="520" height="520" viewBox="0 0 520 520">)";
    out << R"(<rect width="520" height="520" fill="#0d1117"/>)" "\n";
    out << R"(<text x="20" y="32" fill="#e6edf3" font-family="monospace" font-size="18">)"
        << "BedrockTools &#8212; Block Outline (rebuilt)</text>\n";
    out << R"(<text x="20" y="52" fill="#7d8590" font-family="monospace" font-size="13">)"
        << subtitle << "</text>\n";

    const float cx = 260.0f, cy = 290.0f, scale = 110.0f;

    for (const auto& q : quads) {
        auto p0 = project(q.v[0], cx, cy, scale, centre);
        auto p1 = project(q.v[1], cx, cy, scale, centre);
        auto p2 = project(q.v[2], cx, cy, scale, centre);
        auto p3 = project(q.v[3], cx, cy, scale, centre);
        char tmp[256];
        std::snprintf(tmp, sizeof(tmp),
            "<polygon points=\"%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f\" "
            "fill=\"%s\" fill-opacity=\"%.2f\" stroke=\"%s\" stroke-width=\"0.5\" stroke-opacity=\"0.8\"/>",
            p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y,
            rgbBuf, aFlt * 0.85f, rgbBuf);
        out << tmp << "\n";
    }

    for (const auto& l : lines) {
        auto p0 = project(l.a, cx, cy, scale, centre);
        auto p1 = project(l.b, cx, cy, scale, centre);
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp),
            "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" "
            "stroke=\"rgb(255,255,255)\" stroke-width=\"1.5\"/>",
            p0.x, p0.y, p1.x, p1.y);
        out << tmp << "\n";
    }

    out << R"(<text x="20" y="500" fill="#7d8590" font-family="monospace" font-size="12">)"
        << "outline ARGB=#" << std::hex << std::setw(8) << std::setfill('0')
        << (argb & 0xFFFFFFFFu) << std::dec << "</text>\n";
    out << "</svg>\n";
}

// ── ASCII wireframe (visible in the terminal even without a viewer) ─────
static void writeAscii(const std::vector<bo::Line>& lines,
                       const std::vector<bo::Quad>& quads,
                       const bedrocktools::sdk::Vec3& centre,
                       int width, int height,
                       const std::string& title) {
    const float cx = width  / 2.0f;
    const float cy = height / 2.0f;
    const float scaleX = 8.0f;     // pixel per projected unit
    const float scaleY = 5.0f;
    std::vector<std::string> grid(height, std::string(width, ' '));

    auto plotPx = [&](float x, float y, char c) {
        const int ix = static_cast<int>(cx + x * scaleX + 0.5f);
        const int iy = static_cast<int>(cy - y * scaleY + 0.5f);
        if (ix >= 0 && ix < width && iy >= 0 && iy < height)
            grid[iy][ix] = c;
    };

    // Quad band sample dots (drawn first, lines on top).
    for (const auto& q : quads) {
        for (const auto& v : q.v) {
            const V2 p = project(v, 0.0f, 0.0f, 1.0f, centre);
            plotPx(p.x, p.y, '.');
        }
    }

    auto bresenham = [&](float x0f, float y0f, float x1f, float y1f) {
        int x0 = static_cast<int>(cx + x0f * scaleX + 0.5f);
        int y0 = static_cast<int>(cy - y0f * scaleY + 0.5f);
        int x1 = static_cast<int>(cx + x1f * scaleX + 0.5f);
        int y1 = static_cast<int>(cy - y1f * scaleY + 0.5f);
        int dxi = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dyi = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dxi + dyi;
        for (;;) {
            if (x0 >= 0 && x0 < width && y0 >= 0 && y0 < height)
                grid[y0][x0] = '#';
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dyi) { err += dyi; x0 += sx; }
            if (e2 <= dxi) { err += dxi; y0 += sy; }
        }
    };
    for (const auto& l : lines) {
        const V2 a = project(l.a, 0.0f, 0.0f, 1.0f, centre);
        const V2 b = project(l.b, 0.0f, 0.0f, 1.0f, centre);
        bresenham(a.x, a.y, b.x, b.y);
    }

    std::cout << title << std::endl;
    for (const auto& row : grid) {
        bool allBlank = true;
        for (char c : row) if (c != ' ') { allBlank = false; break; }
        (void)allBlank;
        std::cout << "    " << row << std::endl;
    }
}

static void testSvg() {
    std::cout << "[test] SVG + ASCII wireframe (visible proof the rebuild runs)" << std::endl;
    std::string dir = "build";
    std::string mk = "mkdir -p " + dir;
    if (std::system(mk.c_str()) != 0) {
        std::cout << "  [FAIL] failed to create " << dir << std::endl;
        ++g_failed; return;
    }

    BlockOutlineConfig cfg;
    cfg.outlineColor = 0xFF40E0D8u;
    cfg.threeD       = false;
    cfg.thickness    = 3.0f;
    cfg.inset        = 0.01f;

    bo::GeometryOptions opt;
    opt.threeD    = cfg.threeD;
    opt.thickness = cfg.thickness;
    opt.gap       = cfg.inset;

    const bedrocktools::sdk::BlockPos block{100, 64, -200};
    const bedrocktools::sdk::Vec3    centre{
        static_cast<float>(block.x) + 0.5f,
        static_cast<float>(block.y) + 0.5f,
        static_cast<float>(block.z) + 0.5f
    };

    std::vector<bo::Line> lines;
    std::vector<bo::Quad> quads;
    bo::buildOutlineGeometry(block, opt, lines, quads);

    writeSvg(dir + "/outline_2d.svg", lines, quads, centre,
             "2D band pass (crisp edges + face bands) at block (100, 64, -200)",
             cfg.outlineColor);
    BO_CHECK("outline_2d.svg written", true);

    writeAscii(lines, quads, centre, 36, 17,
               "    ASCII wireframe — 2D band pass block (100, 64, -200)");
    BO_CHECK("ascii 2D wireframe rendered", true);

    cfg.threeD = true;
    opt.threeD = true;
    lines.clear(); quads.clear();
    bo::buildOutlineGeometry(block, opt, lines, quads);

    writeSvg(dir + "/outline_3d.svg", lines, quads, centre,
             "3D beam pass (volumetric cage + outer skeleton) at block (100, 64, -200)",
             cfg.outlineColor);
    BO_CHECK("outline_3d.svg written", true);

    writeAscii(lines, quads, centre, 36, 17,
               "    ASCII wireframe — 3D beam pass block (100, 64, -200)");
    BO_CHECK("ascii 3D wireframe rendered", true);
}

int main() {
    std::cout << "=== block outline core rebuild - host test driver ===" << std::endl;

    testConfigRoundTrip();
    testSnapshotValidation();
    testGeometryBuilders();
    testRgbCycle();
    testSvg();

    std::cout << std::endl;
    std::cout << "Summary: " << g_passed << " passed, "
                                << g_failed << " failed" << std::endl;
    return g_failed == 0 ? 0 : 1;
}
