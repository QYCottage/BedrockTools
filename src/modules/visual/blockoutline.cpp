#include "blockoutline.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace {

using TessBeginFn = void (*)(void*, void*, int, int, int);
using TessColorFn = void (*)(void*, float, float, float, float);
using TessVertexFn = void (*)(void*, float, float, float);
using RenderMeshFn = void (*)(void*, void*, void*, char*);
using GetHitResultFn = void* (*)(void*);

struct HashedString {
    uint64_t hash = 0;
    std::string string;
    const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* value) : string(value ? value : "") {
        constexpr uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr uint64_t prime = 0x100000001B3ULL;
        hash = offset;
        for (unsigned char c : string)
            hash = static_cast<uint64_t>(c) ^ (prime * hash);
    }
};

// This is the same small smart-pointer representation already used by the
// working ChunkBorder/Hitbox renderers in this project. We deliberately do
// not construct or destroy the game's material; the game owns it.
struct MaterialPtr {
    void* data[2] = {nullptr, nullptr};
    explicit operator bool() const { return data[0] != nullptr; }
};

static BlockOutlineModule* g_module = nullptr;
static TessBeginFn g_tessBegin = nullptr;
static TessColorFn g_tessColor = nullptr;
static TessVertexFn g_tessVertex = nullptr;
static RenderMeshFn g_renderMesh = nullptr;
static GetHitResultFn g_getHitResult = nullptr;
static void* g_localPlayer = nullptr;
static uintptr_t g_materialGroup = 0;
static MaterialPtr g_selectionMaterial;

static void (*g_renderLevelOriginal)(void*, void*, void*) = nullptr;

static uintptr_t resolveADRP(uint32_t* code, size_t count, uint32_t reg) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t insn = code[i];
        if ((insn & 0x1F) != reg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            const uintptr_t page =
                ((uintptr_t)&code[i] & ~0xFFFULL) +
                ((int64_t)((uint64_t)(((insn >> 3) & 0x1FFFFC) |
                                      ((insn >> 29) & 3)) << 43) >> 31);

            for (size_t j = i + 1; j < count; ++j) {
                const uint32_t add = code[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == reg &&
                    (add & 0x1F) == reg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == reg) break;
            }
        }

        if ((insn & 0x9F000000) == 0x10000000) {
            const int64_t imm =
                (int64_t)((uint64_t)(((insn >> 3) & 0x1FFFFC) |
                                     ((insn >> 29) & 3)) << 43) >> 43;
            return (uintptr_t)&code[i] + imm;
        }
    }
    return 0;
}

static MaterialPtr getMaterial(const char* name) {
    if (!g_materialGroup || !name) return {};

    void** vtable = *reinterpret_cast<void***>(g_materialGroup);
    if (!vtable || !vtable[2]) return {};

    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterialFn>(vtable[2])(
        reinterpret_cast<void*>(g_materialGroup), HashedString(name));
}

static bool validPtr(const void* p) {
    return reinterpret_cast<uintptr_t>(p) >= 0x10000ULL;
}

static uint32_t rgbColor(uint32_t base, float speed) {
    if (!g_module || !g_module->rgb) return base;

    static float hue = 0.0f;
    hue += 0.0015f * std::max(0.0f, speed);
    if (hue >= 1.0f) hue -= std::floor(hue);

    const float h = hue * 6.0f;
    const int i = static_cast<int>(std::floor(h));
    const float f = h - static_cast<float>(i);
    const float q = 1.0f - f;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch (i % 6) {
        case 0: r = 1; g = f; b = 0; break;
        case 1: r = q; g = 1; b = 0; break;
        case 2: r = 0; g = 1; b = f; break;
        case 3: r = 0; g = q; b = 1; break;
        case 4: r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = q; break;
    }

    return (base & 0xFF000000u) |
           (static_cast<uint32_t>(r * 255.0f) << 16) |
           (static_cast<uint32_t>(g * 255.0f) << 8) |
           static_cast<uint32_t>(b * 255.0f);
}

static void addBoxLines(std::vector<std::pair<bedrocktools::sdk::Vec3,
                                               bedrocktools::sdk::Vec3>>& lines,
                        const bedrocktools::sdk::Vec3& min,
                        const bedrocktools::sdk::Vec3& max) {
    const auto p = [](float x, float y, float z) {
        return bedrocktools::sdk::Vec3{x, y, z};
    };

    // Bottom.
    lines.push_back({p(min.x,min.y,min.z), p(max.x,min.y,min.z)});
    lines.push_back({p(max.x,min.y,min.z), p(max.x,min.y,max.z)});
    lines.push_back({p(max.x,min.y,max.z), p(min.x,min.y,max.z)});
    lines.push_back({p(min.x,min.y,max.z), p(min.x,min.y,min.z)});

    // Top.
    lines.push_back({p(min.x,max.y,min.z), p(max.x,max.y,min.z)});
    lines.push_back({p(max.x,max.y,min.z), p(max.x,max.y,max.z)});
    lines.push_back({p(max.x,max.y,max.z), p(min.x,max.y,max.z)});
    lines.push_back({p(min.x,max.y,max.z), p(min.x,max.y,min.z)});

    // Verticals.
    lines.push_back({p(min.x,min.y,min.z), p(min.x,max.y,min.z)});
    lines.push_back({p(max.x,min.y,min.z), p(max.x,max.y,min.z)});
    lines.push_back({p(max.x,min.y,max.z), p(max.x,max.y,max.z)});
    lines.push_back({p(min.x,min.y,max.z), p(min.x,max.y,max.z)});
}

static void updateLocalPlayer(void* player) {
    g_localPlayer = validPtr(player) ? player : nullptr;
}

static void renderBlockOutline(void* levelRenderer, void* screenContext) {
    if (!g_module || !g_module->enabled) return;
    if (!validPtr(levelRenderer) || !validPtr(screenContext)) return;
    if (!validPtr(g_localPlayer)) return;
    if (!g_tessBegin || !g_tessColor || !g_tessVertex || !g_renderMesh || !g_getHitResult)
        return;
    if (!g_selectionMaterial) return;

    const uintptr_t sc = reinterpret_cast<uintptr_t>(screenContext);
    const uintptr_t tessPtr = *reinterpret_cast<uintptr_t*>(
        sc + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (tessPtr < 0x10000ULL) return;

    const uintptr_t lrp = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(levelRenderer) +
        bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (lrp < 0x10000ULL) return;

    const uintptr_t cam = lrp + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos;
    const float camX = *reinterpret_cast<float*>(cam + 0);
    const float camY = *reinterpret_cast<float*>(cam + 4);
    const float camZ = *reinterpret_cast<float*>(cam + 8);
    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ)) return;

    // The player -> Level relationship is already used throughout BedrockTools.
    // We only call LevelGetHitResult after validating the cached player and
    // Level pointer; no guessed render-object pointer chain is used.
    const uintptr_t level = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(g_localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel);
    if (level < 0x10000ULL) return;

    void* hit = g_getHitResult(reinterpret_cast<void*>(level));
    if (!validPtr(hit)) return;

    const int type = *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(hit) + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeBlock) return;

    struct BlockPos { int x, y, z; };
    const auto* block = reinterpret_cast<const BlockPos*>(
        reinterpret_cast<uintptr_t>(hit) + bedrocktools::sdk::offsets::HitResult::mBlockPos);
    if (!block) return;
    if (std::abs(block->x) > 30000000 || std::abs(block->z) > 30000000 ||
        block->y < -2048 || block->y > 4096) return;

    const float hitX = *reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(hit) + bedrocktools::sdk::offsets::HitResult::mPos + 0);
    const float hitY = *reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(hit) + bedrocktools::sdk::offsets::HitResult::mPos + 4);
    const float hitZ = *reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(hit) + bedrocktools::sdk::offsets::HitResult::mPos + 8);
    if (!std::isfinite(hitX) || !std::isfinite(hitY) || !std::isfinite(hitZ)) return;

    const float dx = hitX - camX;
    const float dy = hitY - camY;
    const float dz = hitZ - camZ;
    const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (!std::isfinite(distance) || distance > g_module->range) return;

    const float expand = std::clamp(g_module->lineThickness * 0.0025f, 0.001f, 0.02f);
    const float x0 = static_cast<float>(block->x) - expand;
    const float y0 = static_cast<float>(block->y) - expand;
    const float z0 = static_cast<float>(block->z) - expand;
    const float x1 = static_cast<float>(block->x + 1) + expand;
    const float y1 = static_cast<float>(block->y + 1) + expand;
    const float z1 = static_cast<float>(block->z + 1) + expand;

    std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lines;
    lines.reserve(12);
    addBoxLines(lines, {x0,y0,z0}, {x1,y1,z1});

    uint32_t drawColor = rgbColor(g_module->color, g_module->rgbSpeed) | 0xFF000000u;
    const float r = ((drawColor >> 16) & 0xFFu) / 255.0f;
    const float g = ((drawColor >> 8) & 0xFFu) / 255.0f;
    const float b = (drawColor & 0xFFu) / 255.0f;

    void* tessellator = reinterpret_cast<void*>(tessPtr);
    g_tessBegin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
    g_tessColor(tessellator, r, g, b, 1.0f);

    for (const auto& line : lines) {
        auto p1 = line.first;
        auto p2 = line.second;
        p1.x -= camX; p1.y -= camY; p1.z -= camZ;
        p2.x -= camX; p2.y -= camY; p2.z -= camZ;
        g_tessVertex(tessellator, p1.x, p1.y, p1.z);
        g_tessVertex(tessellator, p2.x, p2.y, p2.z);
    }

    char pad[0x58]{};
    g_renderMesh(screenContext, tessellator, &g_selectionMaterial, pad);
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal)
        g_renderLevelOriginal(self, screenContext, a3);

    // Rendering code is deliberately kept behind a complete pointer/setting
    // check. The original render function always runs first.
    renderBlockOutline(self, screenContext);
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws an outline around the block under the crosshair.") {
    showInMenu = true;
    color = 0xFFFFFFFF;
    rgb = false;
    rgbSpeed = 1.0f;
    lineThickness = 1.0f;
    range = 20.0f;
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    const uintptr_t renderLevel = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel)
        m_patchTarget = reinterpret_cast<void*>(renderLevel);

    g_tessBegin = reinterpret_cast<TessBeginFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::TessellatorBegin));
    g_tessColor = reinterpret_cast<TessColorFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::TessellatorColor));
    g_tessVertex = reinterpret_cast<TessVertexFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::TessellatorVertex));

    uintptr_t rm = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!rm) {
        rm = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    }
    g_renderMesh = reinterpret_cast<RenderMeshFn>(rm);

    g_getHitResult = reinterpret_cast<GetHitResultFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult));

    const uintptr_t rmg = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        const uintptr_t group = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (group)
            g_materialGroup = group + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
    }

    // Never install a render hook unless every primitive needed by the module
    // was resolved. This is the main crash-prevention rule for this module.
    if (m_patchTarget && g_tessBegin && g_tessColor && g_tessVertex &&
        g_renderMesh && g_getHitResult && g_materialGroup) {
        g_selectionMaterial = getMaterial("selection_box");
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateLocalPlayer(event.player); });
}

void BlockOutlineModule::onEnable() {
    if (m_patched || !m_patchTarget || !g_selectionMaterial ||
        !g_tessBegin || !g_tessColor || !g_tessVertex || !g_renderMesh ||
        !g_getHitResult) {
        return;
    }

    if (bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(renderLevelHook),
                                     reinterpret_cast<void**>(&g_renderLevelOriginal))) {
        m_patched = true;
    }
}

void BlockOutlineModule::onDisable() {}

void BlockOutlineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("color")) {
        try {
            const std::string value = j["color"].get<std::string>();
            if (!value.empty()) color = static_cast<uint32_t>(std::stoul(value[0] == '#' ? value.substr(1) : value, nullptr, 16));
        } catch (...) {}
    }
    rgb = j.value("rgb", rgb);
    rgbSpeed = std::clamp(j.value("rgbSpeed", rgbSpeed), 0.0f, 20.0f);
    lineThickness = std::clamp(j.value("lineThickness", lineThickness), 0.5f, 5.0f);
    range = std::clamp(j.value("range", range), 1.0f, 64.0f);
}

void BlockOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    char hex[12];
    std::snprintf(hex, sizeof(hex), "#%08X", color);
    j["color"] = std::string(hex);
    j["rgb"] = rgb;
    j["rgbSpeed"] = rgbSpeed;
    j["lineThickness"] = lineThickness;
    j["range"] = range;
}
