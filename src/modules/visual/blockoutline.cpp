#include "blockoutline.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Level.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

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

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

struct Line {
    bedrocktools::sdk::Vec3 a;
    bedrocktools::sdk::Vec3 b;
};

struct Quad {
    bedrocktools::sdk::Vec3 v[4];
};

static BlockOutlineModule* g_module = nullptr;

static TessellatorBeginFn s_tessBegin = nullptr;
static TessellatorColorFn s_tessColor = nullptr;
static TessellatorVertexFn s_tessVertex = nullptr;
static RenderMeshImmediatelyFn s_renderMesh = nullptr;

static void* s_renderMaterialGroup = nullptr;
static void* g_localPlayer = nullptr;
static MaterialPtr s_selectionMaterial;
static MaterialPtr s_xrayMaterial;

static void (*s_renderLevelOriginal)(void*, void*, void*) = nullptr;

static void hsvToRgb(float h, float s, float v, float& outR, float& outG, float& outB) {
    h = std::fmod(h, 1.0f);
    if (h < 0.0f)
        h += 1.0f;

    const float scaled = h * 6.0f;
    const int sector = static_cast<int>(std::floor(scaled));
    const float fraction = scaled - static_cast<float>(sector);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * fraction);
    const float t = v * (1.0f - s * (1.0f - fraction));

    switch (sector % 6) {
        case 0: outR = v; outG = t; outB = p; break;
        case 1: outR = q; outG = v; outB = p; break;
        case 2: outR = p; outG = v; outB = t; break;
        case 3: outR = p; outG = q; outB = v; break;
        case 4: outR = t; outG = p; outB = v; break;
        default: outR = v; outG = p; outB = q; break;
    }
}

static uint32_t getOutlineColor() {
    if (!g_module || !g_module->rgb)
        return g_module ? g_module->outlineColor : 0xFFFFFFFF;

    g_module->rgbHue = std::fmod(
        g_module->rgbHue + 0.002f * std::max(g_module->rgbSpeed, 0.0f),
        1.0f);

    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    hsvToRgb(g_module->rgbHue, 1.0f, 1.0f, r, g, b);

    const auto toByte = [](float channel) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f);
    };

    return (g_module->outlineColor & 0xFF000000u) |
           (toByte(r) << 16) |
           (toByte(g) << 8) |
           toByte(b);
}

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t insn = insns[i];

        if ((insn & 0x1F) != targetReg)
            continue;

        // ADRP Xn, ...
        if ((insn & 0x9F000000) == 0x90000000) {
            const uintptr_t page =
                ((uintptr_t)&insns[i] & ~0xFFFULL) +
                ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                      (insn >> 29) & 3)
                           << 43) >>
                 31);

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

        // ADR Xn, ...
        if ((insn & 0x9F000000) == 0x10000000) {
            const int64_t imm =
                (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                     (insn >> 29))
                          << 43) >>
                43;

            return (uintptr_t)&insns[i] + imm;
        }
    }

    return 0;
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup || !name)
        return {};

    // BedrockTools uses the same HashedString layout for material lookup.
    struct HashedString {
        uint64_t hash;
        std::string value;
        mutable const HashedString* lastMatch;

        explicit HashedString(const char* text) : hash(0), value(text ? text : ""), lastMatch(nullptr) {
            constexpr uint64_t offsetBasis = 0xCBF29CE484222325ULL;
            constexpr uint64_t prime = 0x100000001B3ULL;

            hash = offsetBasis;
            for (char c : value)
                hash = static_cast<uint64_t>(static_cast<unsigned char>(c)) ^
                       (prime * hash);
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

    // Through-wall / no-depth materials so the 3D frame stays visible
    // behind the block. UI fill is preferred (vertex color, no texture).
    if (!s_xrayMaterial) {
        static const char* kXrayNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
            "particles_alpha",
            "name_tag",
            "ui_textured"
        };
        for (const char* name : kXrayNames) {
            s_xrayMaterial = getMaterial(name);
            if (s_xrayMaterial)
                break;
        }
    }
}

static void addBoxLines(
    std::vector<Line>& lines,
    const bedrocktools::sdk::Vec3& min,
    const bedrocktools::sdk::Vec3& max) {

    // Bottom square.
    lines.push_back({{min.x, min.y, min.z}, {max.x, min.y, min.z}});
    lines.push_back({{max.x, min.y, min.z}, {max.x, min.y, max.z}});
    lines.push_back({{max.x, min.y, max.z}, {min.x, min.y, max.z}});
    lines.push_back({{min.x, min.y, max.z}, {min.x, min.y, min.z}});

    // Top square.
    lines.push_back({{min.x, max.y, min.z}, {max.x, max.y, min.z}});
    lines.push_back({{max.x, max.y, min.z}, {max.x, max.y, max.z}});
    lines.push_back({{max.x, max.y, max.z}, {min.x, max.y, max.z}});
    lines.push_back({{min.x, max.y, max.z}, {min.x, max.y, min.z}});

    // Vertical edges.
    lines.push_back({{min.x, min.y, min.z}, {min.x, max.y, min.z}});
    lines.push_back({{max.x, min.y, min.z}, {max.x, max.y, min.z}});
    lines.push_back({{max.x, min.y, max.z}, {max.x, max.y, max.z}});
    lines.push_back({{min.x, min.y, max.z}, {min.x, max.y, max.z}});
}

// Builds the thick outline as flat bands hugging the six faces of the box.
// The box should sit slightly OUTSIDE the block so depth testing does not
// bury the bands inside the solid. `t` is the band width.
static void addThickOutlineQuads(
    std::vector<Quad>& quads,
    const bedrocktools::sdk::Vec3& min,
    const bedrocktools::sdk::Vec3& max,
    float t) {

    const float x0 = min.x, y0 = min.y, z0 = min.z;
    const float x1 = max.x, y1 = max.y, z1 = max.z;

    // Rectangle on a horizontal (Y-facing) plane.
    const auto rectY = [&](float y, float ax0, float ax1, float az0, float az1) {
        quads.push_back({{
            {ax0, y, az0}, {ax1, y, az0}, {ax1, y, az1}, {ax0, y, az1}
        }});
    };

    // Rectangle on a Z-facing plane.
    const auto rectZ = [&](float z, float ax0, float ax1, float ay0, float ay1) {
        quads.push_back({{
            {ax0, ay0, z}, {ax1, ay0, z}, {ax1, ay1, z}, {ax0, ay1, z}
        }});
    };

    // Rectangle on an X-facing plane.
    const auto rectX = [&](float x, float ay0, float ay1, float az0, float az1) {
        quads.push_back({{
            {x, ay0, az0}, {x, ay1, az0}, {x, ay1, az1}, {x, ay0, az1}
        }});
    };

    // Top and bottom faces: two full-length bands plus two shorter bands so
    // the frame corners are covered exactly once.
    for (float y : {y0, y1}) {
        rectY(y, x0, x1, z0, z0 + t);
        rectY(y, x0, x1, z1 - t, z1);
        rectY(y, x0, x0 + t, z0 + t, z1 - t);
        rectY(y, x1 - t, x1, z0 + t, z1 - t);
    }

    // North/south faces.
    for (float z : {z0, z1}) {
        rectZ(z, x0, x1, y0, y0 + t);
        rectZ(z, x0, x1, y1 - t, y1);
        rectZ(z, x0, x0 + t, y0 + t, y1 - t);
        rectZ(z, x1 - t, x1, y0 + t, y1 - t);
    }

    // East/west faces.
    for (float x : {x0, x1}) {
        rectX(x, y0, y0 + t, z0, z1);
        rectX(x, y1 - t, y1, z0, z1);
        rectX(x, y0 + t, y1 - t, z0, z0 + t);
        rectX(x, y0 + t, y1 - t, z1 - t, z1);
    }
}

// Builds the 3D outline: twelve volumetric beams (rectangular prisms), one
// along every edge of the box. Each beam has a square t x t cross-section
// that extends OUTWARD from the surface. Drawing inside the block makes the
// volume invisible (the solid mesh wins the depth test), so the frame has to
// sit in open air around the edges. Beams are also extended by t on both
// ends so the eight corners stay solid.
static void addBoxBeamQuads(
    std::vector<Quad>& quads,
    const bedrocktools::sdk::Vec3& min,
    const bedrocktools::sdk::Vec3& max,
    float t) {

    const float x0 = min.x, y0 = min.y, z0 = min.z;
    const float x1 = max.x, y1 = max.y, z1 = max.z;

    // Adds the 6 faces of an axis-aligned cuboid.
    const auto cuboid = [&](float ax0, float ay0, float az0,
                            float ax1, float ay1, float az1) {
        // Min/max X faces.
        quads.push_back({{ {ax0, ay0, az0}, {ax0, ay1, az0}, {ax0, ay1, az1}, {ax0, ay0, az1} }});
        quads.push_back({{ {ax1, ay0, az0}, {ax1, ay1, az0}, {ax1, ay1, az1}, {ax1, ay0, az1} }});
        // Min/max Y faces.
        quads.push_back({{ {ax0, ay0, az0}, {ax1, ay0, az0}, {ax1, ay0, az1}, {ax0, ay0, az1} }});
        quads.push_back({{ {ax0, ay1, az0}, {ax1, ay1, az0}, {ax1, ay1, az1}, {ax0, ay1, az1} }});
        // Min/max Z faces.
        quads.push_back({{ {ax0, ay0, az0}, {ax1, ay0, az0}, {ax1, ay1, az0}, {ax0, ay1, az0} }});
        quads.push_back({{ {ax0, ay0, az1}, {ax1, ay0, az1}, {ax1, ay1, az1}, {ax0, ay1, az1} }});
    };

    // Beams parallel to X, one at each (y, z) corner, extending outward.
    for (float y : {y0, y1}) {
        const float ya = (y == y0) ? y - t : y;
        const float yb = (y == y0) ? y : y + t;
        for (float z : {z0, z1}) {
            const float za = (z == z0) ? z - t : z;
            const float zb = (z == z0) ? z : z + t;
            cuboid(x0 - t, ya, za, x1 + t, yb, zb);
        }
    }

    // Beams parallel to Y, one at each (x, z) corner.
    for (float x : {x0, x1}) {
        const float xa = (x == x0) ? x - t : x;
        const float xb = (x == x0) ? x : x + t;
        for (float z : {z0, z1}) {
            const float za = (z == z0) ? z - t : z;
            const float zb = (z == z0) ? z : z + t;
            cuboid(xa, y0 - t, za, xb, y1 + t, zb);
        }
    }

    // Beams parallel to Z, one at each (x, y) corner.
    for (float x : {x0, x1}) {
        const float xa = (x == x0) ? x - t : x;
        const float xb = (x == x0) ? x : x + t;
        for (float y : {y0, y1}) {
            const float ya = (y == y0) ? y - t : y;
            const float yb = (y == y0) ? y : y + t;
            cuboid(xa, ya, z0 - t, xb, yb, z1 + t);
        }
    }
}

static void renderOutline(
    void* screenContext,
    void* tessellator,
    void* lineMaterial,
    void* fillMaterial,
    const std::vector<Line>& lines,
    const std::vector<Quad>& quads,
    float camX,
    float camY,
    float camZ,
    uint32_t color) {

    if (!s_tessBegin || !s_tessColor || !s_tessVertex ||
        !s_renderMesh || !screenContext || !tessellator) {
        return;
    }

    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >> 8) & 0xFF) / 255.0f;
    const float b = (color & 0xFF) / 255.0f;
    const float a = ((color >> 24) & 0xFF) / 255.0f;

    char pad[0x58];

    // Pass 1: thick outline bands drawn as filled quads hugging the block
    // faces. The thickness comes from real geometry (not glLineWidth, which
    // is ignored by most OpenGL ES drivers), so it can be changed freely.
    if (fillMaterial && !quads.empty()) {
        // Quad list; each quad is emitted twice (both windings) so it is
        // visible from either side, matching the Breadcrumbs fill pass.
        s_tessBegin(
            tessellator,
            nullptr,
            1,
            static_cast<int>(quads.size() * 8),
            0);

        s_tessColor(tessellator, r, g, b, a);

        for (const auto& quad : quads) {
            for (int i = 0; i < 4; ++i) {
                s_tessVertex(
                    tessellator,
                    quad.v[i].x - camX,
                    quad.v[i].y - camY,
                    quad.v[i].z - camZ);
            }
            for (int i = 3; i >= 0; --i) {
                s_tessVertex(
                    tessellator,
                    quad.v[i].x - camX,
                    quad.v[i].y - camY,
                    quad.v[i].z - camZ);
            }
        }

        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, fillMaterial, pad);
    }

    // Pass 2: crisp core edges on top of the bands, so the outline keeps a
    // sharp 1px edge even at very small thickness values.
    if (lineMaterial && !lines.empty()) {
        // GL_LINES / BedrockTools tessellator mode used by Hitbox/ChunkBorder.
        s_tessBegin(
            tessellator,
            nullptr,
            4,
            static_cast<int>(lines.size() * 2),
            0);

        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            s_tessVertex(
                tessellator,
                line.a.x - camX,
                line.a.y - camY,
                line.a.z - camZ);
            s_tessVertex(
                tessellator,
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

    if (!g_module || !g_module->enabled)
        return;

    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000)
        return;

    if (!s_tessBegin || !s_tessColor ||
        !s_tessVertex || !s_renderMesh) {
        return;
    }

    const uintptr_t screenAddr = reinterpret_cast<uintptr_t>(screenContext);

    const uintptr_t tessellatorPtr =
        *reinterpret_cast<uintptr_t*>(
            screenAddr + bedrocktools::sdk::offsets::ScreenContext::mTessellator);

    if (!tessellatorPtr || tessellatorPtr < 0x1000)
        return;

    void* tessellator = reinterpret_cast<void*>(tessellatorPtr);

    const uintptr_t rendererAddr = reinterpret_cast<uintptr_t>(self);

    const uintptr_t lrpPtr =
        *reinterpret_cast<uintptr_t*>(
            rendererAddr +
            bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);

    if (!lrpPtr || lrpPtr < 0x1000)
        return;

    const float camX = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);

    const float camY = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);

    const float camZ = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    const uintptr_t localPlayerPtr =
        reinterpret_cast<uintptr_t>(g_localPlayer);

    if (!localPlayerPtr || localPlayerPtr < 0x1000)
        return;

    const uintptr_t levelPtr =
        *reinterpret_cast<uintptr_t*>(
            localPlayerPtr + bedrocktools::sdk::offsets::Actor::mLevel);

    if (!levelPtr || levelPtr < 0x1000)
        return;

    // mHitResultWrapper is a UniqueOwnerPointer. Level::storedHitResult()
    // follows its owned-value pointer before accessing the embedded result;
    // treating the owner itself as HitResult reads unrelated Level fields and
    // makes every real block hit look invalid.
    const auto* hitResult =
        reinterpret_cast<const bedrocktools::sdk::Level*>(levelPtr)
            ->storedHitResult();
    if (!hitResult)
        return;

    if (hitResult->type() !=
        bedrocktools::sdk::offsets::HitResult::TypeBlock) {
        return;
    }

    const auto hitPos = hitResult->position();

    const float distance =
        std::sqrt(
            (hitPos.x - camX) * (hitPos.x - camX) +
            (hitPos.y - camY) * (hitPos.y - camY) +
            (hitPos.z - camZ) * (hitPos.z - camZ));

    if (distance > g_module->maxDistance)
        return;

    // Use the selected block field rather than flooring the surface hit
    // point. In HitResult's arm64 layout mBlock is at +32, before mPos at
    // +44; the previous +56 offset pointed into the entity reference.
    const auto blockPos = hitResult->blockPosition();

    const float x = static_cast<float>(blockPos.x);
    const float y = static_cast<float>(blockPos.y);
    const float z = static_cast<float>(blockPos.z);

    // Sit the outline just OUTSIDE the block. An inset frame is hidden by
    // the block's own depth, which is why 3D used to be invisible.
    const float gap =
        std::max(0.0015f, std::min(g_module->inset, 0.05f));

    bedrocktools::sdk::Vec3 min = {
        x - gap,
        y - gap,
        z - gap
    };

    bedrocktools::sdk::Vec3 max = {
        x + 1.0f + gap,
        y + 1.0f + gap,
        z + 1.0f + gap
    };

    const float thickness =
        std::min(std::max(g_module->thickness, 0.0f) * 0.01f, 0.45f);

    std::vector<Line> lines;
    lines.reserve(12);

    std::vector<Quad> quads;
    if (g_module->threeD) {
        // Chunkier beams so the 3D frame has obvious depth even at the
        // default thickness slider value.
        const float beamT = std::max(thickness, 0.016f);
        quads.reserve(72);
        addBoxBeamQuads(quads, min, max, beamT);
        addBoxLines(
            lines,
            {min.x - beamT, min.y - beamT, min.z - beamT},
            {max.x + beamT, max.y + beamT, max.z + beamT});
    } else {
        addBoxLines(lines, min, max);
        if (thickness > 0.0f) {
            quads.reserve(24);
            addThickOutlineQuads(quads, min, max, thickness);
        }
    }

    ensureMaterials();

    void* overlayMaterial =
        reinterpret_cast<void*>(
            lrpPtr +
            bedrocktools::sdk::offsets::LevelRendererPlayer::
                mSelectionOverlayMaterial);

    void* lineMaterial =
        s_selectionMaterial
            ? reinterpret_cast<void*>(&s_selectionMaterial)
            : overlayMaterial;

    // Filled bands use the embedded selection overlay material, the same
    // material Breadcrumbs uses for its filled quads.
    void* fillMaterial = overlayMaterial ? overlayMaterial : lineMaterial;

    if (!lineMaterial && !fillMaterial)
        return;

    // Preserve the color holder just like the existing BedrockTools render modules.
    const uintptr_t colorHolderPtr =
        *reinterpret_cast<uintptr_t*>(
            screenAddr +
            bedrocktools::sdk::offsets::ScreenContext::mColorHolder);

    if (!colorHolderPtr || colorHolderPtr < 0x1000)
        return;

    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    const float saved[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };

    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const uint32_t color = getOutlineColor();

    // Depth-tested pass: the outward 3D beams / 2D bands stay visible on
    // the outside of the block even if a through-wall material is missing.
    renderOutline(
        screenContext,
        tessellator,
        lineMaterial,
        fillMaterial,
        lines,
        quads,
        camX,
        camY,
        camZ,
        color);

    // 3D extra pass: draw the same geometry without depth so the back
    // edges stay visible through the block (Flarial-style "3D Outline").
    if (g_module->threeD && s_xrayMaterial) {
        void* xray = reinterpret_cast<void*>(&s_xrayMaterial);
        renderOutline(
            screenContext,
            tessellator,
            xray,
            xray,
            lines,
            quads,
            camX,
            camY,
            camZ,
            color);
    }

    colorHolder[0] = saved[0];
    colorHolder[1] = saved[1];
    colorHolder[2] = saved[2];
    colorHolder[3] = saved[3];
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws an outline around the block you are looking at. Enable 3D to see the full frame through the block.") {
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this)
        g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    const uintptr_t renderLevel =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderLevel);

    if (renderLevel)
        m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const uintptr_t tessBegin =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tessBegin)
        s_tessBegin = reinterpret_cast<TessellatorBeginFn>(tessBegin);

    const uintptr_t tessColor =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorColor);
    if (tessColor)
        s_tessColor = reinterpret_cast<TessellatorColorFn>(tessColor);

    const uintptr_t tessVertex =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tessVertex)
        s_tessVertex = reinterpret_cast<TessellatorVertexFn>(tessVertex);

    uintptr_t renderMesh =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);

    if (!renderMesh) {
        renderMesh =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    }

    if (renderMesh)
        s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            g_localPlayer = event.player;
        });

    const uintptr_t materialGroup =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);

    if (materialGroup) {
        // The RenderMaterialGroupCommon signature is ADRP X0 + ADD X0, matching
        // Hitbox / ChunkBorder / Breadcrumbs. Trying X2 first (the old code)
        // resolved the wrong pointer, so selection_box / ui_fill_color never
        // loaded and 3D had no through-wall material.
        uintptr_t groupAddr =
            resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 2, 0);
        if (!groupAddr)
            groupAddr =
                resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 16, 0);
        if (!groupAddr)
            groupAddr =
                resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 64, 2);

        if (groupAddr) {
            s_renderMaterialGroup =
                reinterpret_cast<void*>(
                    groupAddr +
                    bedrocktools::sdk::offsets::MaterialGroup::
                        mRenderMaterialGroupOffset);
        }
    }
}

void BlockOutlineModule::applyPatch() {
    if (m_patched || !m_patchTarget)
        return;

    if (!bedrocktools::hooks::install(
            m_patchTarget,
            reinterpret_cast<void*>(renderLevelHook),
            reinterpret_cast<void**>(&s_renderLevelOriginal))) {
        return;
    }

    m_patched = true;
}

void BlockOutlineModule::onEnable() {
    applyPatch();
}

void BlockOutlineModule::onDisable() {
    // The hook intentionally remains installed, matching the existing
    // BedrockTools visual modules. It becomes a no-op while disabled.
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
                    if (value[0] == '#') value = value.substr(1);
                    else if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) value = value.substr(2);
                    outlineColor = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
                    return true;
                } catch (...) {
                }
            }
        }
        return false;
    };

    if (!parseColor("outlineColor")) {
        if (!parseColor("color")) {
            parseColor("m_outlineColor");
        }
    }

    auto asBool = [](const nlohmann::json& v, bool fallback) -> bool {
        try {
            if (v.is_boolean()) return v.get<bool>();
            if (v.is_number()) return v.get<float>() != 0.0f;
            if (v.is_string()) {
                std::string s = v.get<std::string>();
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (s == "true" || s == "1" || s == "on" || s == "yes") return true;
                if (s == "false" || s == "0" || s == "off" || s == "no") return false;
            }
        } catch (...) {
        }
        return fallback;
    };

    if (j.contains("rgb")) rgb = asBool(j["rgb"], rgb);
    else if (j.contains("rainbow")) rgb = asBool(j["rainbow"], rgb);

    if (j.contains("threeD")) threeD = asBool(j["threeD"], threeD);
    else if (j.contains("3d")) threeD = asBool(j["3d"], threeD);
    else if (j.contains("outline3D")) threeD = asBool(j["outline3D"], threeD);

    if (j.contains("rgbSpeed")) rgbSpeed = j["rgbSpeed"].get<float>();
    else if (j.contains("rainbowSpeed")) rgbSpeed = j["rainbowSpeed"].get<float>();

    if (j.contains("inset")) inset = j["inset"].get<float>();
    else if (j.contains("expand")) inset = j["expand"].get<float>();
    else if (j.contains("m_inset")) inset = j["m_inset"].get<float>();

    if (j.contains("thickness")) thickness = j["thickness"].get<float>();
    else if (j.contains("lineWidth")) thickness = j["lineWidth"].get<float>();
    else if (j.contains("m_thickness")) thickness = j["m_thickness"].get<float>();

    if (j.contains("maxDistance")) maxDistance = j["maxDistance"].get<float>();
    else if (j.contains("range")) maxDistance = j["range"].get<float>();
    else if (j.contains("distance")) maxDistance = j["distance"].get<float>();
    else if (j.contains("m_maxDistance")) maxDistance = j["m_maxDistance"].get<float>();
}

void BlockOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    char color[12];
    std::snprintf(color, sizeof(color), "#%08X", outlineColor);

    j["outlineColor"] = std::string(color);
    j["rgb"] = rgb;
    j["threeD"] = threeD;
    j["rgbSpeed"] = rgbSpeed;
    j["inset"] = inset;
    j["thickness"] = thickness;
    j["maxDistance"] = maxDistance;
}

