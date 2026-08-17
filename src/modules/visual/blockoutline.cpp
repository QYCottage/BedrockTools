#include "blockoutline.hpp"

#include "blockoutline_core.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/HitResult.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace bo = bedrocktools::blockoutline;

// The native cpp talks to in-game tessellator / render-mesh /
// material-group pointers. None of those objects exist off-device,
// so on Linux we just keep them null — the host test driver links
// the core alone and never touches this code.
using TessellatorBeginFn =
    void (*)(void* tessellator, void* debugCallback, int primitiveMode,
             int vertexCount, int noIndices);
using TessellatorColorFn =
    void (*)(void* tessellator, float r, float g, float b, float a);
using TessellatorVertexFn =
    void (*)(void* tessellator, float x, float y, float z);
using RenderMeshImmediatelyFn =
    void (*)(void* screenContext, void* tessellator, void* material, char* pad);

struct MaterialPtr {
    void* sharedPtrData[2];
    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

static BlockOutlineModule* g_module = nullptr;

static TessellatorBeginFn s_tessBegin = nullptr;
static TessellatorColorFn s_tessColor = nullptr;
static TessellatorVertexFn s_tessVertex = nullptr;
static RenderMeshImmediatelyFn s_renderMesh = nullptr;

static void* s_renderMaterialGroup = nullptr;

using LevelGetHitResultFn = void* (*)(void*);
static LevelGetHitResultFn s_levelGetHitResult = nullptr;

static bo::BlockHitSnapshot g_hitSnapshot;
static std::mutex           g_hitSnapshotMutex;

static MaterialPtr s_selectionMaterial;
static MaterialPtr s_xrayMaterial;

static void (*s_renderLevelOriginal)(void*, void*, void*) = nullptr;

// ── Glue: Android-only helpers that drive the core from game memory ────
static void updateHitSnapshot(void* player) {
    bo::BlockHitSnapshot next{};
    if (player && s_levelGetHitResult) {
        const uintptr_t level = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(player) +
            bedrocktools::sdk::offsets::Actor::mLevel);
        if (level >= 0x1000) {
            const auto* hit = reinterpret_cast<const bedrocktools::sdk::HitResult*>(
                s_levelGetHitResult(reinterpret_cast<void*>(level)));
            if (hit && hit->type() == bedrocktools::sdk::offsets::HitResult::TypeBlock) {
                next.blockPos = hit->blockPosition();
                next.hitPos   = hit->position();
                // Pre-existing cpp called validHitSnapshot(next) here
                // with next.valid == false, so the bounds-check predicate
                // (which gates on snapshot.valid) always failed and the
                // outline never appeared. Lift the gate to true before
                // asking the core to re-validate the actual bounds.
                next.valid    = true;
                next.valid    = bo::validHitSnapshot(next);
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_hitSnapshotMutex);
    g_hitSnapshot = next;
}

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg)
            continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            const uintptr_t page =
                ((uintptr_t)&insns[i] & ~0xFFFULL) +
                ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                      (insn >> 29) & 3)
                           << 43) >> 31);
            for (size_t j = i + 1; j < count; ++j) {
                const uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000)
                        imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg)
                    break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            const int64_t imm =
                (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                     (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup || !name)
        return {};

    struct HashedString {
        uint64_t hash;
        std::string value;
        mutable const HashedString* lastMatch;
        explicit HashedString(const char* text) : hash(0), value(text ? text : ""), lastMatch(nullptr) {
            constexpr uint64_t offsetBasis = 0xCBF29CE484222325ULL;
            constexpr uint64_t prime       = 0x100000001B3ULL;
            hash = offsetBasis;
            for (char c : value)
                hash = static_cast<uint64_t>(static_cast<unsigned char>(c)) ^ (prime * hash);
        }
    };

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    const auto getMatIndex =
        bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial;
    if (!vtable || !vtable[getMatIndex])
        return {};

    HashedString hs(name);
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterialFn>(vtable[getMatIndex])(
        s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup)
        return;
    if (!s_selectionMaterial)
        s_selectionMaterial = getMaterial("selection_box");
    if (!s_xrayMaterial) {
        static const char* kXrayNames[] = {
            "ui_fill_color", "ui_textured_and_glcolor",
            "particles_alpha", "name_tag", "ui_textured"
        };
        for (const char* name : kXrayNames) {
            s_xrayMaterial = getMaterial(name);
            if (s_xrayMaterial) break;
        }
    }
}

// Pulls the in-game outline colour, applying the rainbow cycle when the
// module has rgb mode on. The core owns the algorithm; this just hands
// off the live module state.
static uint32_t getOutlineColor() {
    if (!g_module)
        return 0xFFFFFFFFu;
    return bo::cycleRgbColor(
        g_module->outlineColor,
        g_module->rgb,
        g_module->rgbHue,
        g_module->rgbSpeed);
}

static void renderOutline(
    void* screenContext,
    void* tessellator,
    void* lineMaterial,
    void* fillMaterial,
    const std::vector<bo::Line>& lines,
    const std::vector<bo::Quad>& quads,
    float camX, float camY, float camZ,
    uint32_t color) {

    if (!s_tessBegin || !s_tessColor || !s_tessVertex ||
        !s_renderMesh || !screenContext || !tessellator) {
        return;
    }

    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >>  8) & 0xFF) / 255.0f;
    const float b = ( color        & 0xFF) / 255.0f;
    const float a = ((color >> 24) & 0xFF) / 255.0f;

    char pad[0x58];

    if (fillMaterial && !quads.empty()) {
        // Cpp's RenderMeshImmediately expects quad verts emitted twice
        // (CW + CCW), so the band is visible from either facing.
        s_tessBegin(
            tessellator, nullptr, 1, static_cast<int>(quads.size() * 8), 0);
        s_tessColor(tessellator, r, g, b, a);
        for (const auto& quad : quads) {
            for (int i = 0; i < 4; ++i) {
                s_tessVertex(tessellator,
                             quad.v[i].x - camX,
                             quad.v[i].y - camY,
                             quad.v[i].z - camZ);
            }
            for (int i = 3; i >= 0; --i) {
                s_tessVertex(tessellator,
                             quad.v[i].x - camX,
                             quad.v[i].y - camY,
                             quad.v[i].z - camZ);
            }
        }
        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, fillMaterial, pad);
    }

    if (lineMaterial && !lines.empty()) {
        s_tessBegin(
            tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);
        for (const auto& line : lines) {
            s_tessVertex(tessellator,
                         line.a.x - camX,
                         line.a.y - camY,
                         line.a.z - camZ);
            s_tessVertex(tessellator,
                         line.b.x - camX,
                         line.b.y - camY,
                         line.b.z - camZ);
        }
        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, lineMaterial, pad);
    }
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOriginal)
        s_renderLevelOriginal(self, screenContext, a3);

    if (!g_module || !g_module->enabled || !screenContext ||
        reinterpret_cast<uintptr_t>(screenContext) < 0x1000)
        return;

    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh)
        return;

    const uintptr_t screenAddr = reinterpret_cast<uintptr_t>(screenContext);
    const uintptr_t tessellatorPtr = *reinterpret_cast<uintptr_t*>(
        screenAddr + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000)
        return;
    void* tessellator = reinterpret_cast<void*>(tessellatorPtr);

    const uintptr_t rendererAddr = reinterpret_cast<uintptr_t>(self);
    const uintptr_t lrpPtr = *reinterpret_cast<uintptr_t*>(
        rendererAddr + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000)
        return;

    const float camX = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    bo::BlockHitSnapshot hitSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_hitSnapshotMutex);
        hitSnapshot = g_hitSnapshot;
    }
    if (!bo::validHitSnapshot(hitSnapshot))
        return;

    const auto& hitPos = hitSnapshot.hitPos;
    const float distance = std::sqrt(
        (hitPos.x - camX) * (hitPos.x - camX) +
        (hitPos.y - camY) * (hitPos.y - camY) +
        (hitPos.z - camZ) * (hitPos.z - camZ));
    if (distance > g_module->maxDistance)
        return;

    // Build the outline geometry via the shared core. The core owns the
    // gap/thickness/threeD policy; the cpp just hands it the blockPos
    // and the user's slider values.
    bo::GeometryOptions opt;
    opt.threeD    = g_module->threeD;
    opt.thickness = g_module->thickness;
    opt.gap       = g_module->inset;

    std::vector<bo::Line> lines;
    std::vector<bo::Quad> quads;
    bo::buildOutlineGeometry(hitSnapshot.blockPos, opt, lines, quads);

    ensureMaterials();

    void* overlayMaterial = reinterpret_cast<void*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    void* lineMaterial =
        s_selectionMaterial
            ? reinterpret_cast<void*>(&s_selectionMaterial)
            : overlayMaterial;
    void* fillMaterial = overlayMaterial ? overlayMaterial : lineMaterial;
    if (!lineMaterial && !fillMaterial)
        return;

    const uintptr_t colorHolderPtr = *reinterpret_cast<uintptr_t*>(
        screenAddr + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000)
        return;

    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    const float saved[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const uint32_t color = getOutlineColor();

    renderOutline(screenContext, tessellator, lineMaterial, fillMaterial,
                  lines, quads, camX, camY, camZ, color);

    if (g_module->threeD && s_xrayMaterial) {
        void* xray = reinterpret_cast<void*>(&s_xrayMaterial);
        renderOutline(screenContext, tessellator, xray, xray,
                      lines, quads, camX, camY, camZ, color);
    }

    colorHolder[0] = saved[0];
    colorHolder[1] = saved[1];
    colorHolder[2] = saved[2];
    colorHolder[3] = saved[3];
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline",
             "Draws an outline around the block you are looking at. Enable 3D to see the full frame through the block.") {
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    const uintptr_t renderLevel =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel)
        m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const uintptr_t tessBegin =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tessBegin)
        s_tessBegin = reinterpret_cast<TessellatorBeginFn>(tessBegin);

    const uintptr_t tessColor =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tessColor)
        s_tessColor = reinterpret_cast<TessellatorColorFn>(tessColor);

    const uintptr_t tessVertex =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tessVertex)
        s_tessVertex = reinterpret_cast<TessellatorVertexFn>(tessVertex);

    uintptr_t renderMesh =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMesh)
        renderMesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    if (renderMesh)
        s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);

    const uintptr_t getHitResult =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult);
    if (getHitResult)
        s_levelGetHitResult = reinterpret_cast<LevelGetHitResultFn>(getHitResult);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateHitSnapshot(event.player); });

    const uintptr_t materialGroup =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        uintptr_t groupAddr =
            resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 2, 0);
        if (!groupAddr)
            groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 16, 0);
        if (!groupAddr)
            groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 64, 2);
        if (groupAddr) {
            s_renderMaterialGroup = reinterpret_cast<void*>(
                groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset);
        }
    }
}

void BlockOutlineModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    if (!bedrocktools::hooks::install(
            m_patchTarget,
            reinterpret_cast<void*>(renderLevelHook),
            reinterpret_cast<void**>(&s_renderLevelOriginal))) {
        return;
    }
    m_patched = true;
}

void BlockOutlineModule::onEnable() { applyPatch(); }

void BlockOutlineModule::onDisable() {
    // Hook intentionally stays installed; it no-ops while disabled.
}

void BlockOutlineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    auto parseColor = [&](const std::string& key) -> bool {
        if (!j.contains(key)) return false;
        if (j[key].is_number_unsigned() || j[key].is_number_integer()) {
            outlineColor = j[key].get<uint32_t>();
            return true;
        }
        if (j[key].is_string()) {
            std::string value = j[key].get<std::string>();
            if (!value.empty()) {
                try {
                    if      (value[0] == '#') value = value.substr(1);
                    else if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
                        value = value.substr(2);
                    outlineColor = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
                    return true;
                } catch (...) {}
            }
        }
        return false;
    };
    if (!parseColor("outlineColor")) {
        if (!parseColor("color")) parseColor("m_outlineColor");
    }

    auto asBool = [](const nlohmann::json& v, bool fallback) -> bool {
        try {
            if (v.is_boolean()) return v.get<bool>();
            if (v.is_number())  return v.get<float>() != 0.0f;
            if (v.is_string()) {
                std::string s = v.get<std::string>();
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (s == "true"  || s == "1" || s == "on"  || s == "yes") return true;
                if (s == "false" || s == "0" || s == "off" || s == "no")  return false;
            }
        } catch (...) {}
        return fallback;
    };

    if (j.contains("rgb")) rgb = asBool(j["rgb"], rgb);
    else if (j.contains("rainbow")) rgb = asBool(j["rainbow"], rgb);

    if      (j.contains("threeD"))   threeD = asBool(j["threeD"], threeD);
    else if (j.contains("3d"))       threeD = asBool(j["3d"], threeD);
    else if (j.contains("outline3D"))threeD = asBool(j["outline3D"], threeD);

    if      (j.contains("rgbSpeed"))     rgbSpeed = j["rgbSpeed"].get<float>();
    else if (j.contains("rainbowSpeed")) rgbSpeed = j["rainbowSpeed"].get<float>();

    if      (j.contains("inset"))  inset = j["inset"].get<float>();
    else if (j.contains("expand")) inset = j["expand"].get<float>();
    else if (j.contains("m_inset"))inset = j["m_inset"].get<float>();

    if      (j.contains("thickness"))  thickness = j["thickness"].get<float>();
    else if (j.contains("lineWidth"))  thickness = j["lineWidth"].get<float>();
    else if (j.contains("m_thickness"))thickness = j["m_thickness"].get<float>();

    if      (j.contains("maxDistance"))  maxDistance = j["maxDistance"].get<float>();
    else if (j.contains("range"))        maxDistance = j["range"].get<float>();
    else if (j.contains("distance"))     maxDistance = j["distance"].get<float>();
    else if (j.contains("m_maxDistance"))maxDistance = j["m_maxDistance"].get<float>();
}

void BlockOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    char color[12];
    std::snprintf(color, sizeof(color), "#%08X", outlineColor);
    j["outlineColor"] = std::string(color);
    j["rgb"]          = rgb;
    j["threeD"]       = threeD;
    j["rgbSpeed"]     = rgbSpeed;
    j["inset"]        = inset;
    j["thickness"]    = thickness;
    j["maxDistance"]  = maxDistance;
}
