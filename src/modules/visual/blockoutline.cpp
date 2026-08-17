#include "blockoutline.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Level.hpp>

#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace bedrocktools;
using bedrocktools::blockoutline::BlockHitSnapshot;

using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshFn = void (*)(void*, void*, void*, char*);
using RenderLevelFn = void (*)(void*, void*, void*);

struct MaterialPtr {
    void* data[2]{nullptr, nullptr};
    explicit operator bool() const { return data[0] != nullptr; }
};

struct HashedString {
    uint64_t hash = 0;
    std::string value;
    const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* text) : value(text ? text : "") {
        constexpr uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr uint64_t prime = 0x100000001B3ULL;
        hash = offset;
        for (unsigned char c : value)
            hash = c ^ (prime * hash);
    }
};

static BlockOutlineModule* g_module = nullptr;
static TessellatorBeginFn g_begin = nullptr;
static TessellatorColorFn g_color = nullptr;
static TessellatorVertexFn g_vertex = nullptr;
static RenderMeshFn g_renderMesh = nullptr;
static uintptr_t g_materialGroup = 0;
static MaterialPtr g_fillMaterial;
static RenderLevelFn g_renderLevelOriginal = nullptr;

static bool validPtr(uintptr_t p) {
    return p >= 0x10000ULL;
}

static MaterialPtr getMaterial(const char* name) {
    if (!validPtr(g_materialGroup)) return {};
    void** vtable = *reinterpret_cast<void***>(g_materialGroup);
    if (!vtable || !vtable[2]) return {};
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    HashedString key(name);
    return reinterpret_cast<GetMaterialFn>(vtable[2])(
        reinterpret_cast<void*>(g_materialGroup), &key);
}

static void ensureMaterial() {
    if (g_fillMaterial) return;
    // The same vertex-colour capable material order used by Hitbox.
    constexpr const char* names[] = {
        "ui_fill_color",
        "ui_textured_and_glcolor",
        "debug_filled_box",
        "selection_box"
    };
    for (const char* name : names) {
        g_fillMaterial = getMaterial(name);
        if (g_fillMaterial) break;
    }
}

static void updateHitSnapshot(BlockOutlineModule* mod, sdk::Player* player) {
    if (!mod || !player) {
        if (mod) mod->m_hitValid.store(false, std::memory_order_release);
        return;
    }

    const sdk::Vec3 playerPos = player->position();
    mod->m_playerX.store(playerPos.x, std::memory_order_relaxed);
    mod->m_playerY.store(playerPos.y, std::memory_order_relaxed);
    mod->m_playerZ.store(playerPos.z, std::memory_order_relaxed);

    sdk::Level* level = player->level();
    if (!level) {
        mod->m_hitValid.store(false, std::memory_order_release);
        return;
    }

    const sdk::HitResult* hit = level->storedHitResult();
    if (!hit || hit->type() != sdk::offsets::HitResult::TypeBlock) {
        mod->m_hitValid.store(false, std::memory_order_release);
        return;
    }

    const sdk::BlockPos pos = hit->blockPosition();
    const sdk::Vec3 hitPos = hit->position();
    BlockHitSnapshot snapshot{pos, hitPos, true};
    if (!bedrocktools::blockoutline::validHitSnapshot(snapshot)) {
        mod->m_hitValid.store(false, std::memory_order_release);
        return;
    }

    // Publish all fields first; the valid flag is the release/acquire fence.
    mod->m_blockX.store(pos.x, std::memory_order_relaxed);
    mod->m_blockY.store(pos.y, std::memory_order_relaxed);
    mod->m_blockZ.store(pos.z, std::memory_order_relaxed);
    mod->m_hitX.store(hitPos.x, std::memory_order_relaxed);
    mod->m_hitY.store(hitPos.y, std::memory_order_relaxed);
    mod->m_hitZ.store(hitPos.z, std::memory_order_relaxed);
    mod->m_hitValid.store(true, std::memory_order_release);
}

static void drawCameraFacingLines(void* screenContext,
                                  void* tessellator,
                                  void* material,
                                  const std::vector<bedrocktools::blockoutline::Line>& lines,
                                  const bedrocktools::blockoutline::Rgb& rgb,
                                  float lineThickness,
                                  float camX, float camY, float camZ) {
    if (!g_begin || !g_color || !g_vertex || !g_renderMesh || !material || lines.empty()) return;

    const float halfWidth = std::clamp(lineThickness, 0.5f, 20.0f) * 0.005f;
    const bool thick = lineThickness > 1.0f;
    char pad[0x58]{};

    // Thick mode is real geometry rather than glLineWidth, which is ignored
    // or clamped by many Android GLES drivers.
    if (thick) {
        g_begin(tessellator, nullptr, 1, static_cast<int>(lines.size() * 8), 0);
        g_color(tessellator, rgb.r, rgb.g, rgb.b, 1.0f);

        for (const auto& line : lines) {
            float p1x = line.a.x - camX, p1y = line.a.y - camY, p1z = line.a.z - camZ;
            float p2x = line.b.x - camX, p2y = line.b.y - camY, p2z = line.b.z - camZ;

            float dx = p2x - p1x, dy = p2y - p1y, dz = p2z - p1z;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 1e-5f) continue;
            dx /= len; dy /= len; dz /= len;

            const float mx = (p1x + p2x) * 0.5f;
            const float my = (p1y + p2y) * 0.5f;
            const float mz = (p1z + p2z) * 0.5f;

            float sx = dy * mz - dz * my;
            float sy = dz * mx - dx * mz;
            float sz = dx * my - dy * mx;
            float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sl < 1e-5f) {
                if (std::fabs(dy) < 0.9f) {
                    sx = -dz; sy = 0.0f; sz = dx;
                } else {
                    sx = 1.0f; sy = 0.0f; sz = 0.0f;
                }
                sl = std::sqrt(sx * sx + sy * sy + sz * sz);
            }
            sx = sx / sl * halfWidth;
            sy = sy / sl * halfWidth;
            sz = sz / sl * halfWidth;

            const float ex = dx * halfWidth;
            const float ey = dy * halfWidth;
            const float ez = dz * halfWidth;

            const sdk::Vec3 q[4] = {
                {p1x - ex - sx, p1y - ey - sy, p1z - ez - sz},
                {p2x + ex - sx, p2y + ey - sy, p2z + ez - sz},
                {p2x + ex + sx, p2y + ey + sy, p2z + ez + sz},
                {p1x - ex + sx, p1y - ey + sy, p1z - ez + sz}
            };

            for (const auto& v : q) g_vertex(tessellator, v.x, v.y, v.z);
            for (int i = 3; i >= 0; --i) g_vertex(tessellator, q[i].x, q[i].y, q[i].z);
        }
        g_renderMesh(screenContext, tessellator, material, pad);
    }

    // Hairline pass guarantees a crisp edge when the thick quad falls below
    // a pixel at distance.
    g_begin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
    g_color(tessellator, rgb.r, rgb.g, rgb.b, 1.0f);
    for (const auto& line : lines) {
        g_vertex(tessellator, line.a.x - camX, line.a.y - camY, line.a.z - camZ);
        g_vertex(tessellator, line.b.x - camX, line.b.y - camY, line.b.z - camZ);
    }
    g_renderMesh(screenContext, tessellator, material, pad);
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal)
        g_renderLevelOriginal(self, screenContext, a3);

    BlockOutlineModule* mod = g_module;
    if (!mod || !mod->enabled || !screenContext || !validPtr(reinterpret_cast<uintptr_t>(screenContext))) return;
    if (!g_begin || !g_color || !g_vertex || !g_renderMesh) return;
    if (!mod->m_hitValid.load(std::memory_order_acquire)) return;

    const int bx = mod->m_blockX.load(std::memory_order_relaxed);
    const int by = mod->m_blockY.load(std::memory_order_relaxed);
    const int bz = mod->m_blockZ.load(std::memory_order_relaxed);
    const float hx = mod->m_hitX.load(std::memory_order_relaxed);
    const float hy = mod->m_hitY.load(std::memory_order_relaxed);
    const float hz = mod->m_hitZ.load(std::memory_order_relaxed);

    const float px = mod->m_playerX.load(std::memory_order_relaxed);
    const float py = mod->m_playerY.load(std::memory_order_relaxed);
    const float pz = mod->m_playerZ.load(std::memory_order_relaxed);
    const float dx = hx - px, dy = hy - py, dz = hz - pz;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance) || distance > std::max(1.0f, mod->maxDistance)) return;

    const uintptr_t sc = reinterpret_cast<uintptr_t>(screenContext);
    const uintptr_t tessPtr = *reinterpret_cast<const uintptr_t*>(sc + sdk::offsets::ScreenContext::mTessellator);
    if (!validPtr(tessPtr)) return;

    const uintptr_t lrp = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(self) + sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!validPtr(lrp)) return;

    const auto& cam = *reinterpret_cast<const sdk::Vec3*>(lrp + sdk::offsets::LevelRendererPlayer::mCamPos);

    ensureMaterial();
    void* material = g_fillMaterial ? reinterpret_cast<void*>(&g_fillMaterial)
                                    : reinterpret_cast<void*>(lrp + sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    if (!material) return;

    uintptr_t colorHolderPtr = *reinterpret_cast<const uintptr_t*>(sc + sdk::offsets::ScreenContext::mColorHolder);
    if (!validPtr(colorHolderPtr)) return;
    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    const float saved[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = colorHolder[1] = colorHolder[2] = colorHolder[3] = 1.0f;

    uint32_t color = mod->outlineColor;
    if (mod->rgb)
        color = bedrocktools::blockoutline::cycleRgbColor(color, true, mod->m_rgbHue, mod->rgbSpeed);
    const auto channels = bedrocktools::blockoutline::unpackArgb(color);

    std::vector<bedrocktools::blockoutline::Line> lines;
    std::vector<bedrocktools::blockoutline::Quad> unused;
    bedrocktools::blockoutline::GeometryOptions opt;
    opt.thickness = mod->lineThickness;
    opt.threeD = mod->threeD;
    bedrocktools::blockoutline::buildOutlineGeometry({bx, by, bz}, opt, lines, unused);

    drawCameraFacingLines(screenContext, reinterpret_cast<void*>(tessPtr), material,
                          lines, channels, mod->lineThickness,
                          cam.x, cam.y, cam.z);

    // 3D mode uses the volumetric beam quads generated by the shared core.
    // This second pass keeps the far edges visible through the block.
    if (mod->threeD && !unused.empty() && g_begin && g_color && g_vertex && g_renderMesh) {
        char pad[0x58]{};
        g_begin(reinterpret_cast<void*>(tessPtr), nullptr, 1,
                static_cast<int>(unused.size() * 8), 0);
        g_color(reinterpret_cast<void*>(tessPtr), channels.r, channels.g, channels.b, 1.0f);
        for (const auto& quad : unused) {
            for (int i = 0; i < 4; ++i) {
                const auto& v = quad.v[i];
                g_vertex(reinterpret_cast<void*>(tessPtr), v.x - cam.x, v.y - cam.y, v.z - cam.z);
            }
            for (int i = 3; i >= 0; --i) {
                const auto& v = quad.v[i];
                g_vertex(reinterpret_cast<void*>(tessPtr), v.x - cam.x, v.y - cam.y, v.z - cam.z);
            }
        }
        g_renderMesh(screenContext, reinterpret_cast<void*>(tessPtr), material, pad);
    }

    std::memcpy(colorHolder, saved, sizeof(saved));
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Highlights the block under the crosshair with configurable RGB color and line size") {
    showInMenu = true;
    hideInHudEditor = true;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    if (m_initialized) return;
    m_initialized = true;
    g_module = this;

    const uintptr_t render = memory::resolve(memory::SignatureId::RenderLevel);
    const uintptr_t begin = memory::resolve(memory::SignatureId::TessellatorBegin);
    const uintptr_t color = memory::resolve(memory::SignatureId::TessellatorColor);
    const uintptr_t vertex = memory::resolve(memory::SignatureId::TessellatorVertex);
    uintptr_t mesh = memory::resolve(memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!mesh) mesh = memory::resolve(memory::SignatureId::MeshHelpersRenderMeshImmediately);
    const uintptr_t group = memory::resolve(memory::SignatureId::RenderMaterialGroupCommon);

    m_renderTarget = reinterpret_cast<void*>(render);
    g_begin = reinterpret_cast<TessellatorBeginFn>(begin);
    g_color = reinterpret_cast<TessellatorColorFn>(color);
    g_vertex = reinterpret_cast<TessellatorVertexFn>(vertex);
    g_renderMesh = reinterpret_cast<RenderMeshFn>(mesh);

    if (group) {
        // RenderMaterialGroupCommon loads the singleton pointer using ADRP + ADD.
        const auto* insns = reinterpret_cast<const uint32_t*>(group);
        uintptr_t page = reinterpret_cast<uintptr_t>(insns) & ~0xFFFULL;
        const uint32_t first = insns[0];
        if ((first & 0x9F000000u) == 0x90000000u) {
            const int64_t imm = ((static_cast<int64_t>((first >> 5) & 0x7FFFF) << 2) |
                                 static_cast<int64_t>((first >> 29) & 0x3));
            const int64_t signedImm = (imm & (1LL << 20)) ? (imm | ~((1LL << 21) - 1)) : imm;
            const uintptr_t adrp = page + (signedImm << 12);
            const uint32_t add = insns[1];
            if ((add & 0xFF000000u) == 0x91000000u)
                g_materialGroup = adrp + ((add >> 10) & 0xFFFu);
        }
        if (!g_materialGroup) {
            // Fallback: RenderLevel already contains the selection material,
            // so material lookup is optional. The renderer can still work.
            g_materialGroup = 0;
        }
    }

    events::bus().subscribe<events::LocalPlayerTickEvent>(
        [this](auto& event) { updateHitSnapshot(this, event.player); });

    verifyRuntime();
}

void BlockOutlineModule::installRenderHook() {
    if (m_hookInstalled || !m_renderTarget || !runtimeReady()) return;
    if (hooks::install(m_renderTarget, reinterpret_cast<void*>(renderLevelHook),
                       reinterpret_cast<void**>(&g_renderLevelOriginal))) {
        m_hookInstalled = true;
    }
}

bool BlockOutlineModule::runtimeReady() const {
    return m_renderTarget && g_begin && g_color && g_vertex && g_renderMesh;
}

void BlockOutlineModule::verifyRuntime() {
    const bool render = m_renderTarget != nullptr;
    const bool begin = g_begin != nullptr;
    const bool color = g_color != nullptr;
    const bool vertex = g_vertex != nullptr;
    const bool mesh = g_renderMesh != nullptr;
    const bool all = render && begin && color && vertex && mesh;

    if (all) {
        m_verificationStatus = "PASS - RenderLevel + Tessellator + Mesh resolved";
    } else if (!render) {
        m_verificationStatus = "FAIL - RenderLevel signature not found";
    } else if (!begin) {
        m_verificationStatus = "FAIL - TessellatorBegin signature not found";
    } else if (!color) {
        m_verificationStatus = "FAIL - TessellatorColor signature not found";
    } else if (!vertex) {
        m_verificationStatus = "FAIL - TessellatorVertex signature not found";
    } else {
        m_verificationStatus = "FAIL - RenderMesh signature not found";
    }
}

void BlockOutlineModule::onEnable() {
    verifyRuntime();
    installRenderHook();
}

void BlockOutlineModule::onDisable() {
    // Hooks are intentionally left installed. The hook checks enabled before
    // doing any rendering, matching the lifecycle used by the other render
    // modules and avoiding an unsafe runtime unpatch on Android.
}

void BlockOutlineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("outlineColor")) {
        if (j["outlineColor"].is_string()) {
            try {
                std::string s = j["outlineColor"].get<std::string>();
                if (!s.empty() && s[0] == '#') s.erase(0, 1);
                if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s.erase(0, 2);
                outlineColor = static_cast<uint32_t>(std::stoul(s, nullptr, 16));
            } catch (...) {}
        } else if (j["outlineColor"].is_number_unsigned() || j["outlineColor"].is_number_integer()) {
            outlineColor = j["outlineColor"].get<uint32_t>();
        }
    } else if (j.contains("color")) {
        try { outlineColor = static_cast<uint32_t>(std::stoul(j["color"].get<std::string>(), nullptr, 16)); } catch (...) {}
    }

    rgb = j.value("rgb", j.value("rainbow", rgb));
    rgbSpeed = std::clamp(j.value("rgbSpeed", j.value("rainbowSpeed", rgbSpeed)), 0.05f, 5.0f);
    lineThickness = std::clamp(j.value("lineThickness", j.value("thickness", lineThickness)), 0.5f, 20.0f);
    maxDistance = std::clamp(j.value("maxDistance", j.value("range", maxDistance)), 1.0f, 180.0f);
    threeD = j.value("threeD", j.value("3d", threeD));

    verifyButton = j.value("verifyButton", false);
    if (verifyButton) {
        verifyRuntime();
        verifyButton = false;
    }
}

void BlockOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    char color[16];
    std::snprintf(color, sizeof(color), "#%08X", outlineColor);
    j["outlineColor"] = std::string(color);
    j["rgb"] = rgb;
    j["rgbSpeed"] = rgbSpeed;
    j["lineThickness"] = lineThickness;
    j["maxDistance"] = maxDistance;
    j["threeD"] = threeD;
    j["verifyButton"] = verifyButton;
    j["verificationStatus"] = m_verificationStatus;
}
