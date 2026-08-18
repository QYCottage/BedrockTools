#include "connectedglass.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

using bedrocktools::sdk::field;

enum class GlassFace : uint8_t { Down = 0, Up, North, South, West, East };
enum class TextureEdge : uint8_t { U0, U1, V0, V1 };
enum class GlassShape : uint8_t { None, Block, Pane };

struct Vec3Raw { float x, y, z; };

struct BlockPosRaw {
    int32_t x, y, z;
    friend BlockPosRaw operator+(const BlockPosRaw& a, const BlockPosRaw& b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
};
using BlockOffset = BlockPosRaw;

struct GlassInfo {
    GlassShape shape = GlassShape::None;
    std::string_view name;
    const void* identity = nullptr;
    bool valid() const { return shape != GlassShape::None; }
};

struct CropEdges {
    bool u0 = false, u1 = false, v0 = false, v1 = false;
    bool any() const { return u0 || u1 || v0 || v1; }
};

struct UvBounds { float u0, v0, u1, v1; };

struct PaneRenderContext {
    bool active = false;
    void* tess = nullptr;
    BlockPosRaw pos{};
    CropEdges eastWest{}, northSouth{}, vertical{};
    UvBounds source{};
    bool haveSource = false;
};

using FaceFn = void (*)(void*, void*, const void*, const Vec3Raw*, const void*);
using PaneTessFn = bool (*)(void*, void*, const void*, const BlockPosRaw*, int32_t);
using BgGetTextureFn = const void* (*)(void*, const BlockPosRaw*, size_t, uint32_t);
using TessVertexFn = void (*)(void*, float, float, float);
using BoUpdateRenderFaceFn = uint64_t (*)(void*, const void*, const BlockPosRaw*, const void*, uint8_t);
using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPosRaw*, int32_t);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const BlockPosRaw*);
using TextureUVCopyCtorFn = void* (*)(void*, const void*);
using TextureUVDtorFn = void (*)(void*);
using SetAllDirtyFn = void (*)(void*, bool, bool);

std::atomic_bool g_enabled{false};
std::atomic_bool g_rebuildPending{false};
std::atomic_bool g_connectGlassBlocks{true};
std::atomic_bool g_connectGlassPanes{true};
std::atomic_bool g_connectDifferentColors{true};
std::atomic_bool g_connectPanesToBlocks{true};
std::atomic_bool g_removeFlatBorders{true};
std::atomic_bool g_removeCornerBorders{true};
std::atomic_bool g_removeOuterVerticalBorders{false};
std::atomic_bool g_removeOuterHorizontalBorders{false};
std::atomic_bool g_removeOuterTopBottomFaceBorders{false};
std::atomic_bool g_affectSideFaces{true};
std::atomic_bool g_affectTopFace{true};
std::atomic_bool g_affectBottomFace{true};
std::atomic<float> g_borderWidth{2.0f};

struct FaceHook {
    bedrocktools::hooks::Handle handle = nullptr;
    FaceFn original = nullptr;
};
FaceHook g_faceHooks[6] = {};

bedrocktools::hooks::Handle g_paneTessHook = nullptr;
bedrocktools::hooks::Handle g_bgGetTextureHook = nullptr;
bedrocktools::hooks::Handle g_tessVertexHook = nullptr;
bedrocktools::hooks::Handle g_boUpdateRenderFaceHook = nullptr;

PaneTessFn g_paneTessOriginal = nullptr;
BgGetTextureFn g_bgGetTextureOriginal = nullptr;
TessVertexFn g_tessVertexOriginal = nullptr;
BoUpdateRenderFaceFn g_boUpdateRenderFaceOriginal = nullptr;

BlockSourceGetBlockFn g_getBlock = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
TextureUVCopyCtorFn g_textureCopyCtor = nullptr;
TextureUVDtorFn g_textureDtor = nullptr;
SetAllDirtyFn g_setAllDirty = nullptr;

thread_local PaneRenderContext g_paneRc;

std::string_view stripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

GlassInfo classifyGlassName(std::string_view rawName, const void* identity = nullptr) {
    const std::string_view name = stripNamespace(rawName);
    const bool pane = name == "glass_pane"
        || name == "hard_glass_pane"
        || name == "stained_glass_pane"
        || name == "hard_stained_glass_pane"
        || name.ends_with("_stained_glass_pane");
    const bool block = name == "glass"
        || name == "tinted_glass"
        || name == "hard_glass"
        || name == "stained_glass"
        || name == "hard_stained_glass"
        || name.ends_with("_stained_glass");
    if (pane) return {GlassShape::Pane, name, identity};
    if (block) return {GlassShape::Block, name, identity};
    return {};
}

GlassInfo classifyGlass(const void* block) {
    if (!block) return {};
    const auto* blockObj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* fullName = blockObj->fullName();
    if (!fullName || fullName->size() > 256
        || (!fullName->empty() && fullName->data() == nullptr)) return {};
    return classifyGlassName({fullName->data(), fullName->size()}, block);
}

std::string_view shapeIndependentName(const GlassInfo& info) {
    std::string_view result = info.name;
    constexpr std::string_view paneSuffix = "_pane";
    if (info.shape == GlassShape::Pane && result.ends_with(paneSuffix)) {
        result.remove_suffix(paneSuffix.size());
    }
    return result;
}

bool shapeEnabled(const GlassInfo& info) {
    if (info.shape == GlassShape::Block) return g_connectGlassBlocks.load(std::memory_order_relaxed);
    if (info.shape == GlassShape::Pane) return g_connectGlassPanes.load(std::memory_order_relaxed);
    return false;
}

bool compatibleGlass(const GlassInfo& current, const GlassInfo& neighbor) {
    if (!current.valid() || !neighbor.valid() || !shapeEnabled(current) || !shapeEnabled(neighbor)) return false;
    if (current.shape != neighbor.shape && !g_connectPanesToBlocks.load(std::memory_order_relaxed)) return false;
    if (g_connectDifferentColors.load(std::memory_order_relaxed)) return true;
    if (current.shape == neighbor.shape && current.identity && neighbor.identity) {
        return current.identity == neighbor.identity;
    }
    return shapeIndependentName(current) == shapeIndependentName(neighbor);
}

constexpr BlockOffset kFaceNormals[6] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
};

const void* getBlock(void* region, const BlockPosRaw& pos) {
    return region && g_getBlock ? g_getBlock(region, &pos, 0) : nullptr;
}

bool faceEnabled(GlassFace face) {
    if (face == GlassFace::Up) return g_affectTopFace.load(std::memory_order_relaxed);
    if (face == GlassFace::Down) return g_affectBottomFace.load(std::memory_order_relaxed);
    return g_affectSideFaces.load(std::memory_order_relaxed);
}

bool removeOuterBorder(GlassFace face, TextureEdge edge) {
    if (face == GlassFace::Up || face == GlassFace::Down) {
        return g_removeOuterTopBottomFaceBorders.load(std::memory_order_relaxed);
    }
    if (edge == TextureEdge::U0 || edge == TextureEdge::U1) {
        return g_removeOuterVerticalBorders.load(std::memory_order_relaxed);
    }
    return g_removeOuterHorizontalBorders.load(std::memory_order_relaxed);
}

bool isFaceExposed(void* region, const BlockPosRaw& pos, GlassFace face) {
    const BlockPosRaw outsidePos = pos + kFaceNormals[static_cast<size_t>(face)];
    const void* outsideBlock = getBlock(region, outsidePos);
    if (!outsideBlock || classifyGlass(outsideBlock).valid()) return false;
    return g_isSolidBlockingBlock && !g_isSolidBlockingBlock(region, &outsidePos);
}

bool shouldCropEdge(
    void* region, const BlockPosRaw& pos, const GlassInfo& current,
    GlassFace face, TextureEdge edge, const BlockOffset& neighborOffset
) {
    const BlockPosRaw neighborPos = pos + neighborOffset;
    const void* neighborBlock = getBlock(region, neighborPos);
    if (neighborBlock) {
        const GlassInfo neighbor = classifyGlass(neighborBlock);
        if (compatibleGlass(current, neighbor)) {
            if (face == GlassFace::Down || face == GlassFace::Up || isFaceExposed(region, neighborPos, face)) {
                return g_removeFlatBorders.load(std::memory_order_relaxed);
            }
            return g_removeCornerBorders.load(std::memory_order_relaxed);
        }
    }
    return removeOuterBorder(face, edge);
}

constexpr BlockOffset kEdgeDirs[6][4] = {
    {{-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}},   // Down
    {{-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}},   // Up
    {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}},   // North
    {{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}},   // South
    {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}},   // West
    {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}},   // East
};

CropEdges getCropEdges(void* region, const BlockPosRaw& pos, const GlassInfo& current, GlassFace face) {
    if (!faceEnabled(face)) return {};
    const auto& dirs = kEdgeDirs[static_cast<size_t>(face)];
    return {
        shouldCropEdge(region, pos, current, face, TextureEdge::U0, dirs[0]),
        shouldCropEdge(region, pos, current, face, TextureEdge::U1, dirs[1]),
        shouldCropEdge(region, pos, current, face, TextureEdge::V0, dirs[2]),
        shouldCropEdge(region, pos, current, face, TextureEdge::V1, dirs[3]),
    };
}

bool paneNeighborConnected(void* region, const BlockPosRaw& pos, const GlassInfo& current, int dx, int dz) {
    return compatibleGlass(current, classifyGlass(getBlock(region, {pos.x + dx, pos.y, pos.z + dz})));
}

CropEdges paneCropAxis(void* region, const BlockPosRaw& pos, const GlassInfo& current, int dx0, int dz0, int dx1, int dz1) {
    return {
        paneNeighborConnected(region, pos, current, dx0, dz0),
        paneNeighborConnected(region, pos, current, dx1, dz1),
        false, false,
    };
}

CropEdges paneCropVertical(void* region, const BlockPosRaw& pos, const GlassInfo& current) {
    const auto connected = [&](int dy) {
        return compatibleGlass(current, classifyGlass(getBlock(region, {pos.x, pos.y + dy, pos.z})));
    };
    return {false, false, connected(1), connected(-1)};
}

float remapCroppedUv(float value, float start, float end, bool cropStart, bool cropEnd) {
    const float borderPixels = std::clamp(g_borderWidth.load(std::memory_order_relaxed), 0.0f, 7.5f);
    constexpr float logicalTextureSize = 16.0f;
    if (std::abs(end - start) < 1.0e-7f) return value;
    const float pixel = (end - start) / logicalTextureSize;
    const float croppedStart = start + (cropStart ? pixel * borderPixels : 0.0f);
    const float croppedEnd = end - (cropEnd ? pixel * borderPixels : 0.0f);
    return croppedStart + (value - start) * (croppedEnd - croppedStart) / (end - start);
}

bool inUvRange(float value, float a, float b) {
    constexpr float epsilon = 1.0e-5f;
    return value >= std::min(a, b) - epsilon && value <= std::max(a, b) + epsilon;
}

class TextureCopy final {
public:
    explicit TextureCopy(const void* source) {
        if (g_textureCopyCtor && source) {
            g_textureCopyCtor(m_storage.data(), source);
            m_constructed = true;
        }
    }
    ~TextureCopy() {
        if (m_constructed && g_textureDtor) g_textureDtor(m_storage.data());
    }
    TextureCopy(const TextureCopy&) = delete;
    TextureCopy& operator=(const TextureCopy&) = delete;
    bool valid() const { return m_constructed; }
    void* data() { return m_storage.data(); }

private:
    alignas(bedrocktools::sdk::offsets::TextureUVCoordinateSet::Alignment)
        std::array<std::byte, bedrocktools::sdk::offsets::TextureUVCoordinateSet::Size> m_storage{};
    bool m_constructed = false;
};

void cropTexture(void* texture, const CropEdges& edges) {
    float& u0 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU0);
    float& v0 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV0);
    float& u1 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU1);
    float& v1 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV1);

    const float borderPixels = std::clamp(g_borderWidth.load(std::memory_order_relaxed), 0.0f, 7.5f);
    if (borderPixels <= 0.0f) return;

    constexpr float logicalTextureSize = 16.0f;
    const float uPixel = (u1 - u0) / logicalTextureSize;
    const float vPixel = (v1 - v0) / logicalTextureSize;

    if (edges.u0) u0 += uPixel * borderPixels;
    if (edges.u1) u1 -= uPixel * borderPixels;
    if (edges.v0) v0 += vPixel * borderPixels;
    if (edges.v1) v1 -= vPixel * borderPixels;

    field<uint8_t>(
        texture,
        bedrocktools::sdk::offsets::TextureUVCoordinateSet::mIsotropicFaceData
            + bedrocktools::sdk::offsets::IsotropicFaceData::mTextureIsotropic
    ) = 0;
}

class FaceStateGuard final {
public:
    FaceStateGuard(void* tessellator, GlassFace face)
        : m_flip(&field<uint8_t>(
              tessellator,
              bedrocktools::sdk::offsets::BlockTessellator::mFlipFace
                  + static_cast<size_t>(face) * bedrocktools::sdk::offsets::FlipFace::ElementSize
          )),
          m_xFlip(&field<uint8_t>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mXFlipTexture)),
          m_oldFlip(*m_flip), m_oldXFlip(*m_xFlip) {
        *m_flip = bedrocktools::sdk::offsets::FlipFace::DontRotate;
        *m_xFlip = 0;
    }
    ~FaceStateGuard() { *m_flip = m_oldFlip; *m_xFlip = m_oldXFlip; }
    FaceStateGuard(const FaceStateGuard&) = delete;
    FaceStateGuard& operator=(const FaceStateGuard&) = delete;

private:
    uint8_t* m_flip;
    uint8_t* m_xFlip;
    uint8_t m_oldFlip;
    uint8_t m_oldXFlip;
};

void renderFace(
    FaceFn original, GlassFace face, void* tessellator, void* meshTessellator,
    const void* block, const Vec3Raw* position, const void* inputTexture
) {
    if (!original) return;

    std::optional<TextureCopy> texture;
    do {
        if (!g_enabled.load(std::memory_order_relaxed)
            || !tessellator || !block || !position || !inputTexture
            || !g_getBlock || !g_isSolidBlockingBlock
            || !g_textureCopyCtor || !g_textureDtor) break;
        const GlassInfo current = classifyGlass(block);
        if (!current.valid() || !shapeEnabled(current)) break;
        const void* internalTexture = reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(tessellator) + bedrocktools::sdk::offsets::BlockTessellator::mInternalTexture
        );
        if (field<uint8_t>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mUseInternalTexture) != 0
            && inputTexture == internalTexture) break;
        void* region = field<void*>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mRegion);
        if (!region) break;
        const BlockPosRaw pos = {
            static_cast<int32_t>(std::floor(position->x)),
            static_cast<int32_t>(std::floor(position->y)),
            static_cast<int32_t>(std::floor(position->z)),
        };
        const GlassInfo worldBlock = classifyGlass(getBlock(region, pos));
        if (!worldBlock.valid()) break;
        const CropEdges edges = getCropEdges(region, pos, current, face);
        if (!edges.any()) break;
        texture.emplace(inputTexture);
        if (!texture->valid()) { texture.reset(); break; }
        cropTexture(texture->data(), edges);
    } while (false);

    if (!texture) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }
    FaceStateGuard state(tessellator, face);
    original(tessellator, meshTessellator, block, position, texture->data());
}

template <GlassFace Face>
void faceHookTrampoline(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_faceHooks[static_cast<size_t>(Face)].original, Face, a0, a1, a2, a3, a4);
}

constexpr std::array<bedrocktools::memory::SignatureId, 6> kFaceSigIds = {
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceDown,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceUp,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceNorth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceSouth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceWest,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceEast,
};

constexpr std::array<FaceFn, 6> kFaceTrampolines = {
    &faceHookTrampoline<GlassFace::Down>,
    &faceHookTrampoline<GlassFace::Up>,
    &faceHookTrampoline<GlassFace::North>,
    &faceHookTrampoline<GlassFace::South>,
    &faceHookTrampoline<GlassFace::West>,
    &faceHookTrampoline<GlassFace::East>,
};

bool paneTessDetour(void* tess, void* meshtess, const void* block, const BlockPosRaw* pos, int32_t layer) {
    if (!g_paneTessOriginal) return false;

    bool shouldIntercept = false;
    if (g_enabled.load(std::memory_order_relaxed)
        && tess && block && pos && g_getBlock && g_isSolidBlockingBlock) {
        const GlassInfo current = classifyGlass(block);
        if (current.valid() && current.shape == GlassShape::Pane
            && g_connectGlassPanes.load(std::memory_order_relaxed)) {
            shouldIntercept = true;
        }
    }

    const PaneRenderContext saved = g_paneRc;
    if (shouldIntercept) {
        void* region = field<void*>(tess, bedrocktools::sdk::offsets::BlockTessellator::mRegion);
        const GlassInfo current = classifyGlass(block);
        g_paneRc = {
            true, meshtess, *pos,
            paneCropAxis(region, *pos, current, -1, 0, 1, 0),
            paneCropAxis(region, *pos, current, 0, 1, 0, -1),
            paneCropVertical(region, *pos, current),
            {}, false,
        };
    }
    const bool result = g_paneTessOriginal(tess, meshtess, block, pos, layer);
    g_paneRc = saved;
    return result;
}

const void* bgGetTextureDetour(void* bgThis, const BlockPosRaw* pos, size_t faceIdx, uint32_t variant) {
    if (!g_bgGetTextureOriginal) return nullptr;
    const void* result = g_bgGetTextureOriginal(bgThis, pos, faceIdx, variant);
    if (!g_paneRc.active || !result) return result;
    if (faceIdx == 0 && !g_paneRc.haveSource) {
        g_paneRc.source = {
            field<float>(result, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU0),
            field<float>(result, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV0),
            field<float>(result, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU1),
            field<float>(result, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV1),
        };
        g_paneRc.haveSource = true;
    }
    return result;
}

void tessVertexDetour(void* tess, float x, [[maybe_unused]] float y, float z) {
    if (!g_tessVertexOriginal) return;
    if (g_paneRc.active && g_paneRc.haveSource && g_paneRc.tess == tess) {
        float& u = field<float>(tess, bedrocktools::sdk::offsets::Tessellator::mTextureU);
        float& v = field<float>(tess, bedrocktools::sdk::offsets::Tessellator::mTextureV);
        const UvBounds& src = g_paneRc.source;
        if (inUvRange(u, src.u0, src.u1) && inUvRange(v, src.v0, src.v1)) {
            const float dx = std::abs(x - (static_cast<float>(g_paneRc.pos.x) + 0.5f));
            const float dz = std::abs(z - (static_cast<float>(g_paneRc.pos.z) + 0.5f));
            if (std::abs(dx - dz) > 0.01f) {
                const CropEdges& edges = dx > dz ? g_paneRc.eastWest : g_paneRc.northSouth;
                u = remapCroppedUv(u, src.u0, src.u1, edges.u0, edges.u1);
            }
            v = remapCroppedUv(v, src.v0, src.v1, g_paneRc.vertical.v0, g_paneRc.vertical.v1);
        }
    }
    g_tessVertexOriginal(tess, x, y, z);
}

uint64_t boUpdateRenderFaceDetour(
    void* self, const void* block, const BlockPosRaw* pos, const void* aabb, uint8_t face
) {
    if (!g_boUpdateRenderFaceOriginal) return 0;
    const uint64_t result = g_boUpdateRenderFaceOriginal(self, block, pos, aabb, face);
    if (!g_enabled.load(std::memory_order_relaxed) || !g_paneRc.active || !pos) return result;
    if (face != static_cast<uint8_t>(GlassFace::Up) && face != static_cast<uint8_t>(GlassFace::Down)) return result;

    const GlassInfo current = classifyGlass(block);
    if (!current.valid() || current.shape != GlassShape::Pane
        || !g_connectGlassPanes.load(std::memory_order_relaxed)) {
        return result;
    }
    const bool connected = (face == static_cast<uint8_t>(GlassFace::Up))
        ? g_paneRc.vertical.v0
        : g_paneRc.vertical.v1;
    if (!connected) return result;

    const uint64_t bit = 1ULL << face;
    field<uint64_t>(self, 0) |= bit;
    return result | bit;
}

template <typename Fn>
bedrocktools::hooks::Handle installHook(bedrocktools::memory::SignatureId id, void* detour, Fn* original) {
    const uintptr_t address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;
    return bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, reinterpret_cast<void**>(original));
}

bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance || !g_setAllDirty) return false;

    void* levelRenderer = field<void*>(clientInstance, bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer);
    if (!levelRenderer) return false;

    void* node = field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );

    bool rebuilt = false;
    size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = field<void*>(node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = field<void*>(node, bedrocktools::sdk::offsets::HashNode::mValuePointer);
        if (coordinator) {
            g_setAllDirty(coordinator, true, false);
            rebuilt = true;
        }
        node = next;
    }
    return rebuilt;
}

}

void ConnectedGlassHandleClientInstanceUpdate(void* clientInstance) {
    if (!g_rebuildPending.load(std::memory_order_acquire)) return;
    if (rebuildRenderChunks(clientInstance)) {
        g_rebuildPending.store(false, std::memory_order_release);
    }
}

ConnectedGlassModule::ConnectedGlassModule()
    : Module(
          "Connected Glass",
          "Connects glass blocks and panes with configurable borders and colors."
      ) {
}

ConnectedGlassModule::~ConnectedGlassModule() {
    g_enabled.store(false, std::memory_order_relaxed);
}

void ConnectedGlassModule::applySettings() {
    g_connectGlassBlocks.store(connectGlassBlocks, std::memory_order_relaxed);
    g_connectGlassPanes.store(connectGlassPanes, std::memory_order_relaxed);
    g_connectDifferentColors.store(connectDifferentColors, std::memory_order_relaxed);
    g_connectPanesToBlocks.store(connectPanesToBlocks, std::memory_order_relaxed);
    g_removeFlatBorders.store(removeFlatBorders, std::memory_order_relaxed);
    g_removeCornerBorders.store(removeCornerBorders, std::memory_order_relaxed);
    g_removeOuterVerticalBorders.store(removeOuterVerticalBorders, std::memory_order_relaxed);
    g_removeOuterHorizontalBorders.store(removeOuterHorizontalBorders, std::memory_order_relaxed);
    g_removeOuterTopBottomFaceBorders.store(removeOuterTopBottomFaceBorders, std::memory_order_relaxed);
    g_affectSideFaces.store(affectSideFaces, std::memory_order_relaxed);
    g_affectTopFace.store(affectTopFace, std::memory_order_relaxed);
    g_affectBottomFace.store(affectBottomFace, std::memory_order_relaxed);
    borderWidth = std::clamp(borderWidth, 0.0f, 7.5f);
    g_borderWidth.store(borderWidth, std::memory_order_relaxed);
}

void ConnectedGlassModule::installHooks() {
    if (m_hooked) return;

    g_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlockForTessellation));
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    g_textureCopyCtor = reinterpret_cast<TextureUVCopyCtorFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TextureUVCoordinateSetCopyCtor));
    g_textureDtor = reinterpret_cast<TextureUVDtorFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TextureUVCoordinateSetDtor));
    g_setAllDirty = reinterpret_cast<SetAllDirtyFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty));

    for (size_t i = 0; i < 6; ++i) {
        if (!g_faceHooks[i].handle) {
            g_faceHooks[i].handle = installHook(
                kFaceSigIds[i],
                reinterpret_cast<void*>(kFaceTrampolines[i]),
                &g_faceHooks[i].original
            );
        }
    }

    if (!g_paneTessHook) {
        g_paneTessHook = installHook(
            bedrocktools::memory::SignatureId::BlockTessellatorTessellateDoubleThinFenceInWorld,
            reinterpret_cast<void*>(paneTessDetour), &g_paneTessOriginal);
    }
    if (!g_bgGetTextureHook) {
        g_bgGetTextureHook = installHook(
            bedrocktools::memory::SignatureId::BlockGraphicsGetTexture,
            reinterpret_cast<void*>(bgGetTextureDetour), &g_bgGetTextureOriginal);
    }
    if (!g_tessVertexHook) {
        g_tessVertexHook = installHook(
            bedrocktools::memory::SignatureId::TessellatorVertex,
            reinterpret_cast<void*>(tessVertexDetour), &g_tessVertexOriginal);
    }
    if (!g_boUpdateRenderFaceHook) {
        g_boUpdateRenderFaceHook = installHook(
            bedrocktools::memory::SignatureId::BlockOccluderUpdateRenderFace,
            reinterpret_cast<void*>(boUpdateRenderFaceDetour), &g_boUpdateRenderFaceOriginal);
    }

    m_hooked = g_getBlock && g_isSolidBlockingBlock && g_textureCopyCtor && g_textureDtor && g_setAllDirty;
    for (const auto& h : g_faceHooks) m_hooked = m_hooked && h.handle;
    m_hooked = m_hooked && g_paneTessHook && g_bgGetTextureHook && g_tessVertexHook && g_boUpdateRenderFaceHook;
}

void ConnectedGlassModule::onInit() {
    applySettings();
    installHooks();
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        ConnectedGlassHandleClientInstanceUpdate(event.clientInstance);
    });
}

void ConnectedGlassModule::onEnable() {
    applySettings();
    if (!m_hooked) installHooks();
    g_enabled.store(m_hooked, std::memory_order_release);
    if (m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void ConnectedGlassModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    if (m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void ConnectedGlassModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    connectGlassBlocks = j.value("connectGlassBlocks", connectGlassBlocks);
    connectGlassPanes = j.value("connectGlassPanes", connectGlassPanes);
    connectDifferentColors = j.value("connectDifferentColors", connectDifferentColors);
    connectPanesToBlocks = j.value("connectPanesToBlocks", connectPanesToBlocks);
    removeFlatBorders = j.value("removeFlatBorders", removeFlatBorders);
    removeCornerBorders = j.value("removeCornerBorders", removeCornerBorders);
    removeOuterVerticalBorders = j.value("removeOuterVerticalBorders", removeOuterVerticalBorders);
    removeOuterHorizontalBorders = j.value("removeOuterHorizontalBorders", removeOuterHorizontalBorders);
    removeOuterTopBottomFaceBorders = j.value("removeOuterTopBottomFaceBorders", removeOuterTopBottomFaceBorders);
    affectSideFaces = j.value("affectSideFaces", affectSideFaces);
    affectTopFace = j.value("affectTopFace", affectTopFace);
    affectBottomFace = j.value("affectBottomFace", affectBottomFace);
    borderWidth = j.value("borderWidth", borderWidth);
    applySettings();
    if (enabled && m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void ConnectedGlassModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["connectGlassBlocks"] = connectGlassBlocks;
    j["connectGlassPanes"] = connectGlassPanes;
    j["connectDifferentColors"] = connectDifferentColors;
    j["connectPanesToBlocks"] = connectPanesToBlocks;
    j["removeFlatBorders"] = removeFlatBorders;
    j["removeCornerBorders"] = removeCornerBorders;
    j["removeOuterVerticalBorders"] = removeOuterVerticalBorders;
    j["removeOuterHorizontalBorders"] = removeOuterHorizontalBorders;
    j["removeOuterTopBottomFaceBorders"] = removeOuterTopBottomFaceBorders;
    j["affectSideFaces"] = affectSideFaces;
    j["affectTopFace"] = affectTopFace;
    j["affectBottomFace"] = affectBottomFace;
    j["borderWidth"] = borderWidth;
}
