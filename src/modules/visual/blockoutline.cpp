#include "blockoutline.hpp"
#include "blockoutline_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using TessellatorBegin = void (*)(void*, void*, int, int, int);
using TessellatorColor = void (*)(void*, float, float, float, float);
using TessellatorVertex = void (*)(void*, float, float, float);
using RenderMeshImmediately = void (*)(void*, void*, void*, char*);
using RenderLevel = void (*)(void*, void*, void*);
using LevelGetHitResult = void* (*)(void*);

// Tessellator primitive modes used by Bedrock: 1 = quad list (4 vertices per
// quad), 4 = line list (2 vertices per line).
constexpr int kQuadPrimitive = 1;
constexpr int kLinePrimitive = 4;

struct HashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static std::uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t hash = kOffset;
        for (char ch : str) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^
                   (kPrime * hash);
        }
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

static std::uintptr_t resolveADRP(std::uint32_t* insns, size_t count, std::uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            std::uintptr_t page =
                ((std::uintptr_t)&insns[i] & ~0xFFFULL) +
                ((std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC |
                                               (insn >> 29) & 3)
                                << 43) >>
                 31);

            for (size_t j = i + 1; j < count; j++) {
                std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg && (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            std::int64_t imm =
                (std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC |
                                              (insn >> 29))
                               << 43) >>
                43;
            return (std::uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

BlockOutlineModule* g_module = nullptr;
TessellatorBegin g_tessellatorBegin = nullptr;
TessellatorColor g_tessellatorColor = nullptr;
TessellatorVertex g_tessellatorVertex = nullptr;
RenderMeshImmediately g_renderMesh = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
LevelGetHitResult g_getHitResult = nullptr;

std::uintptr_t g_renderMaterialGroup = 0;
MaterialPtr g_matSelection;
MaterialPtr g_matFill;

std::mutex g_targetMutex;
bedrocktools::sdk::BlockPos g_target{};
bool g_hasTarget = false;

void clearTarget() {
    std::lock_guard lock(g_targetMutex);
    g_hasTarget = false;
}

void updateTarget(bedrocktools::sdk::Player* player) {
    if (!g_module || !g_module->enabled || !player || !g_getHitResult) {
        clearTarget();
        return;
    }

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    void* level = *reinterpret_cast<void**>(
        playerAddress + bedrocktools::sdk::offsets::Actor::mLevel);
    void* hitResult = level ? g_getHitResult(level) : nullptr;
    if (!hitResult) {
        clearTarget();
        return;
    }

    const auto hitAddress = reinterpret_cast<std::uintptr_t>(hitResult);
    const int type = *reinterpret_cast<const int*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeBlock) {
        clearTarget();
        return;
    }

    const auto position = *reinterpret_cast<const bedrocktools::sdk::BlockPos*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mBlockPos);
    std::lock_guard lock(g_targetMutex);
    g_target = position;
    g_hasTarget = true;
}

MaterialPtr getMaterial(const char* name) {
    if (!g_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(g_renderMaterialGroup);
    if (!vtable ||
        !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) {
        return {};
    }

    using GetMaterial = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterial>(
        vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])(
        reinterpret_cast<void*>(g_renderMaterialGroup), &hs);
}

void ensureMaterials() {
    if (!g_renderMaterialGroup) return;

    if (!g_matSelection) g_matSelection = getMaterial("selection_box");

    // Filled geometry (3D box faces and thick line quads) needs a
    // vertex-color material; the selection overlay is built for a translucent
    // block highlight and washes the color out once the outline is no longer
    // a hairline.
    if (!g_matFill) {
        static const char* kFillNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
            "debug_filled_box",
            "selection_box",
        };
        for (const char* name : kFillNames) {
            g_matFill = getMaterial(name);
            if (g_matFill) break;
        }
    }
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    }

    if (!g_module || !g_module->enabled || !screenContext || !levelRenderer ||
        !g_tessellatorBegin || !g_tessellatorColor || !g_tessellatorVertex ||
        !g_renderMesh) {
        return;
    }

    bedrocktools::sdk::BlockPos target{};
    {
        std::lock_guard lock(g_targetMutex);
        if (!g_hasTarget) return;
        target = g_target;
    }

    const auto contextAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    void* tessellator = *reinterpret_cast<void**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    float* colorHolder = *reinterpret_cast<float**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (reinterpret_cast<std::uintptr_t>(tessellator) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(colorHolder) < 0x1000) {
        return;
    }

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    void* playerRenderer = *reinterpret_cast<void**>(
        rendererAddress + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (reinterpret_cast<std::uintptr_t>(playerRenderer) < 0x1000) return;

    const auto playerRendererAddress = reinterpret_cast<std::uintptr_t>(playerRenderer);
    const auto camera = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(
        playerRendererAddress + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    // The game's own selection material is always initialized with the level
    // renderer and has the depth/blend state expected for a block outline.
    void* overlayMaterial = reinterpret_cast<void*>(
        playerRendererAddress +
        bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    ensureMaterials();
    void* matInner = g_matSelection ? static_cast<void*>(&g_matSelection) : overlayMaterial;
    void* matFill = g_matFill ? static_cast<void*>(&g_matFill) : matInner;

    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const std::uint32_t color = g_module->outlineColor;
    const float red = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    const float green = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    const float blue = static_cast<float>(color & 0xFFu) / 255.0f;

    const float camX = camera.x;
    const float camY = camera.y;
    const float camZ = camera.z;

    // Line size slider -> world-space half width. 1.0 (or lower) keeps the
    // classic hairline box; above that the edges are drawn as real quads,
    // because GL line width is ignored by nearly every mobile GLES driver.
    float lineSize = g_module->lineThickness;
    if (lineSize < 1.0f) lineSize = 1.0f;
    if (lineSize > 10.0f) lineSize = 10.0f;
    const bool thickLines = lineSize > 1.05f;
    const float halfWidth = lineSize * 0.01f * 0.5f;

    char meshParams[0x58];
    const auto lines = bedrocktools::modules::blockoutline::makeBox(
        static_cast<float>(target.x),
        static_cast<float>(target.y),
        static_cast<float>(target.z));

    // 3D fill pass: translucent faces so the block reads as a solid volume.
    // The faces are expanded slightly (less than the wireframe) so they never
    // z-fight with the block's own surface.
    if (g_module->outline3d) {
        constexpr float kFillAlpha = 0.25f;
        const auto faces = bedrocktools::modules::blockoutline::makeFaces(
            static_cast<float>(target.x),
            static_cast<float>(target.y),
            static_cast<float>(target.z),
            0.001f);

        // 6 faces, both windings (12 quads = 48 vertices) so back-face
        // culling can never eat a face.
        g_tessellatorBegin(tessellator, nullptr, kQuadPrimitive, 6 * 2 * 4, 0);
        g_tessellatorColor(tessellator, red, green, blue, kFillAlpha);
        for (const auto& face : faces) {
            const bedrocktools::sdk::Vec3 verts[4] = {
                {face.a.x - camX, face.a.y - camY, face.a.z - camZ},
                {face.b.x - camX, face.b.y - camY, face.b.z - camZ},
                {face.c.x - camX, face.c.y - camY, face.c.z - camZ},
                {face.d.x - camX, face.d.y - camY, face.d.z - camZ},
            };
            for (int i = 0; i < 4; ++i) {
                g_tessellatorVertex(tessellator, verts[i].x, verts[i].y, verts[i].z);
            }
            for (int i = 3; i >= 0; --i) {
                g_tessellatorVertex(tessellator, verts[i].x, verts[i].y, verts[i].z);
            }
        }
        std::memset(meshParams, 0, sizeof(meshParams));
        g_renderMesh(screenContext, tessellator, matFill, meshParams);
    }

    // Thick pass: every edge becomes a camera-facing quad so the apparent
    // width follows the line-size setting from any angle.
    if (thickLines) {
        g_tessellatorBegin(tessellator, nullptr, kQuadPrimitive,
                           static_cast<int>(lines.size() * 8), 0);
        g_tessellatorColor(tessellator, red, green, blue, 1.0f);

        for (const auto& line : lines) {
            bedrocktools::sdk::Vec3 p1 = {
                line.from.x - camX, line.from.y - camY, line.from.z - camZ};
            bedrocktools::sdk::Vec3 p2 = {
                line.to.x - camX, line.to.y - camY, line.to.z - camZ};

            float dx = p2.x - p1.x;
            float dy = p2.y - p1.y;
            float dz = p2.z - p1.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 1e-5f) continue;
            dx /= len; dy /= len; dz /= len;

            // The camera sits at the origin of this relative space, so the
            // vector to the segment midpoint is the view direction.
            const float mx = (p1.x + p2.x) * 0.5f;
            const float my = (p1.y + p2.y) * 0.5f;
            const float mz = (p1.z + p2.z) * 0.5f;

            // side = dir x view, perpendicular to both the segment and the
            // eye ray => the quad always faces the player.
            float sx = dy * mz - dz * my;
            float sy = dz * mx - dx * mz;
            float sz = dx * my - dy * mx;
            float sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sLen < 1e-5f) {
                // Looking straight down the segment: pick any perpendicular.
                if (std::fabs(dy) < 0.9f) { sx = -dz; sy = 0.0f; sz = dx; }
                else { sx = 1.0f; sy = 0.0f; sz = 0.0f; }
                sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (sLen < 1e-5f) continue;
            }
            sx = sx / sLen * halfWidth;
            sy = sy / sLen * halfWidth;
            sz = sz / sLen * halfWidth;

            // Overshoot both ends by half the width so corners stay solid.
            const float ex = dx * halfWidth;
            const float ey = dy * halfWidth;
            const float ez = dz * halfWidth;

            const bedrocktools::sdk::Vec3 quad[4] = {
                {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz},
            };

            // Emitted with both windings so back-face culling never eats a
            // segment.
            for (int i = 0; i < 4; ++i) {
                g_tessellatorVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }
            for (int i = 3; i >= 0; --i) {
                g_tessellatorVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }
        }

        std::memset(meshParams, 0, sizeof(meshParams));
        g_renderMesh(screenContext, tessellator, matFill, meshParams);
    }

    // Core hairline pass: keeps the edge crisp and visible even when the
    // quads shrink below a pixel at long range.
    g_tessellatorBegin(tessellator, nullptr, kLinePrimitive,
                       static_cast<int>(lines.size() * 2), 0);
    g_tessellatorColor(tessellator, red, green, blue, 1.0f);
    for (const auto& line : lines) {
        g_tessellatorVertex(tessellator,
            line.from.x - camX, line.from.y - camY, line.from.z - camZ);
        g_tessellatorVertex(tessellator,
            line.to.x - camX, line.to.y - camY, line.to.z - camZ);
    }

    std::memset(meshParams, 0, sizeof(meshParams));
    g_renderMesh(screenContext, tessellator, matInner, meshParams);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws a configurable outline around the block you are looking at.") {
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    using bedrocktools::memory::SignatureId;

    m_renderLevel = reinterpret_cast<void*>(
        bedrocktools::memory::resolve(SignatureId::RenderLevel));
    g_tessellatorBegin = reinterpret_cast<TessellatorBegin>(
        bedrocktools::memory::resolve(SignatureId::TessellatorBegin));
    g_tessellatorColor = reinterpret_cast<TessellatorColor>(
        bedrocktools::memory::resolve(SignatureId::TessellatorColor));
    g_tessellatorVertex = reinterpret_cast<TessellatorVertex>(
        bedrocktools::memory::resolve(SignatureId::TessellatorVertex));

    auto renderMeshAddress = bedrocktools::memory::resolve(
        SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMeshAddress) {
        renderMeshAddress = bedrocktools::memory::resolve(
            SignatureId::MeshHelpersRenderMeshImmediately);
    }
    g_renderMesh = reinterpret_cast<RenderMeshImmediately>(renderMeshAddress);
    g_getHitResult = reinterpret_cast<LevelGetHitResult>(
        bedrocktools::memory::resolve(SignatureId::LevelGetHitResult));

    const auto renderMaterialGroup = bedrocktools::memory::resolve(
        SignatureId::RenderMaterialGroupCommon);
    if (renderMaterialGroup) {
        const auto groupAddress = resolveADRP(
            reinterpret_cast<std::uint32_t*>(renderMaterialGroup), 2, 0);
        if (groupAddress) {
            g_renderMaterialGroup =
                groupAddress +
                bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateTarget(event.player); });
}

void BlockOutlineModule::installRenderHook() {
    if (m_hookInstalled || !m_renderLevel) return;
    const auto handle = bedrocktools::hooks::install(
        m_renderLevel,
        reinterpret_cast<void*>(renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    m_hookInstalled = handle != nullptr;
}

void BlockOutlineModule::onEnable() {
    installRenderHook();
}

void BlockOutlineModule::onDisable() {
    clearTarget();
}

void BlockOutlineModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);

    outline3d = json.value("outline3d", outline3d);

    if (json.contains("lineThickness")) {
        try {
            lineThickness = json["lineThickness"].get<float>();
        } catch (...) {
            // Preserve the last valid thickness.
        }
    }
    if (lineThickness < 1.0f) lineThickness = 1.0f;
    if (lineThickness > 10.0f) lineThickness = 10.0f;

    auto readChannel = [&](const char* key, int fallback) -> int {
        if (!json.contains(key) || !json[key].is_number_integer()) return fallback;
        int value = json[key].get<int>();
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        return value;
    };

    const bool hasRgb = json.contains("outlineRed") ||
                        json.contains("outlineGreen") ||
                        json.contains("outlineBlue");
    if (hasRgb) {
        outlineRed = readChannel("outlineRed", outlineRed);
        outlineGreen = readChannel("outlineGreen", outlineGreen);
        outlineBlue = readChannel("outlineBlue", outlineBlue);
        outlineColor = 0xFF000000u |
                       (static_cast<std::uint32_t>(outlineRed) << 16) |
                       (static_cast<std::uint32_t>(outlineGreen) << 8) |
                       static_cast<std::uint32_t>(outlineBlue);
    } else if (json.contains("outlineColor") && json["outlineColor"].is_string()) {
        // Legacy configs only stored the hex picker value.
        std::string value = json["outlineColor"].get<std::string>();
        if (!value.empty() && value.front() == '#') value.erase(value.begin());
        try {
            const auto parsed = std::stoul(value, nullptr, 16);
            // Six-digit colors are RGB; eight-digit colors are AARRGGBB.
            outlineColor = value.size() <= 6
                ? (0xFF000000u | static_cast<std::uint32_t>(parsed))
                : static_cast<std::uint32_t>(parsed);
            outlineRed = static_cast<int>((outlineColor >> 16) & 0xFFu);
            outlineGreen = static_cast<int>((outlineColor >> 8) & 0xFFu);
            outlineBlue = static_cast<int>(outlineColor & 0xFFu);
        } catch (...) {
            // Preserve the last valid color.
        }
    }
}

void BlockOutlineModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);

    // Keep alpha opaque so the line color never washes out.
    outlineColor |= 0xFF000000u;

    json["outline3d"] = outline3d;
    json["lineThickness"] = lineThickness;
    json["outlineRed"] = outlineRed;
    json["outlineGreen"] = outlineGreen;
    json["outlineBlue"] = outlineBlue;
}
