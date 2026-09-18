#include "ColoredLightsFeature.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrocktools::feature::coloredlights {
namespace {

struct BlockPosRaw {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

struct Rgb {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

struct ColorRaw {
    float r;
    float g;
    float b;
    float a;
};

struct ColoredSource {
    BlockPosRaw position{};
    Rgb color{};
    float emission = 0.0f;
};

struct PreparedSource {
    ColoredSource source{};
    float transmission = 1.0f;
};

struct SourceCellKey {
    void* region = nullptr;
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const SourceCellKey& other) const {
        return region == other.region && x == other.x && y == other.y && z == other.z;
    }
};

struct SourceCellKeyHash {
    std::size_t operator()(const SourceCellKey& key) const {
        std::size_t value = std::hash<void*>{}(key.region);
        const auto combine = [&value](int component) {
            value ^= std::hash<int>{}(component) + 0x9E3779B9u + (value << 6) + (value >> 2);
        };
        combine(key.x);
        combine(key.y);
        combine(key.z);
        return value;
    }
};

struct TintContext {
    void* tessellator = nullptr;
    const std::vector<PreparedSource>* sources = nullptr;
    bool active = false;
};

struct TintSample {
    Rgb color{};
    float amount = 0.0f;
    bool active = false;
};

using TessellateBlockFn = bool (*)(
    void*,
    void*,
    const void*,
    const BlockPosRaw*,
    std::uint64_t,
    const void*
);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorColorObjectFn = void (*)(void*, const ColorRaw*);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderChunkBuilderBuildFn = void (*)(
    void*,
    void*,
    bool,
    std::int32_t,
    bool,
    const void*
);
using UseNewTessellationFn = bool (*)(const void*, bool);
using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPosRaw*, std::int32_t);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const BlockPosRaw*);
using BrightnessChangedFn = void (*)(void*, void*, const BlockPosRaw*);
using SetAllDirtyFn = void (*)(void*, bool, bool);

bedrocktools::hooks::Handle g_tessellateBlockHook = nullptr;
bedrocktools::hooks::Handle g_colorHook = nullptr;
bedrocktools::hooks::Handle g_colorObjectHook = nullptr;
bedrocktools::hooks::Handle g_vertexHook = nullptr;
bedrocktools::hooks::Handle g_useNewTessellationHook = nullptr;
bedrocktools::hooks::Handle g_renderChunkBuilderBuildHook = nullptr;
bedrocktools::hooks::Handle g_brightnessHook = nullptr;
TessellateBlockFn g_tessellateBlockOriginal = nullptr;
TessellatorColorFn g_colorOriginal = nullptr;
TessellatorColorObjectFn g_colorObjectOriginal = nullptr;
TessellatorVertexFn g_vertexOriginal = nullptr;
UseNewTessellationFn g_useNewTessellationOriginal = nullptr;
RenderChunkBuilderBuildFn g_renderChunkBuilderBuildOriginal = nullptr;
BrightnessChangedFn g_brightnessOriginal = nullptr;
BlockSourceGetBlockFn g_getBlock = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
SetAllDirtyFn g_setAllDirty = nullptr;

std::atomic_bool g_enabled{false};
std::atomic_int g_rebuildDelayUpdates{-1};
std::atomic_int g_cacheBypassUpdates{0};
std::atomic<float> g_colorStrength{0.85f};
std::atomic<float> g_radiusScale{1.0f};
std::atomic_bool g_wallAttenuation{false};
std::atomic<std::uint32_t> g_redstoneColor{0xFF321F};
std::atomic<std::uint32_t> g_soulColor{0x438DFF};
std::atomic<std::uint32_t> g_seaColor{0x59D7FF};
std::atomic<std::uint32_t> g_warmColor{0xFFB45C};
std::atomic<std::uint32_t> g_purpleColor{0xB970FF};
std::atomic<std::uint32_t> g_greenColor{0x72F29A};
std::atomic<std::uint64_t> g_cacheEpoch{1};
std::atomic<void*> g_lastClientInstance{nullptr};

std::mutex g_sourceCacheMutex;
std::unordered_map<SourceCellKey, std::vector<ColoredSource>, SourceCellKeyHash> g_sourceCache;
thread_local TintContext g_tintContext;
thread_local bool g_insideTerrainBuild = false;

int floorDiv16(int value) {
    return value >= 0 ? value / 16 : -((-value + 15) / 16);
}

bool samePosition(const BlockPosRaw& a, const BlockPosRaw& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

Rgb unpackColor(std::uint32_t color) {
    return {
        static_cast<float>((color >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((color >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(color & 0xFFu) / 255.0f,
    };
}

std::uint32_t parseColor(std::string_view value, std::uint32_t fallback) {
    if (!value.empty() && value.front() == '#') value.remove_prefix(1);
    if (value.size() != 6 && value.size() != 8) return fallback;
    std::uint32_t result = 0;
    for (char ch : value) {
        result <<= 4;
        if (ch >= '0' && ch <= '9') result |= static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') result |= static_cast<std::uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') result |= static_cast<std::uint32_t>(ch - 'A' + 10);
        else return fallback;
    }
    if (value.size() == 8) result >>= 8;
    return result & 0xFFFFFFu;
}

std::string normalizeColor(std::string value, std::uint32_t fallback) {
    const std::uint32_t color = parseColor(value, fallback);
    char buffer[10]{};
    std::snprintf(buffer, sizeof(buffer), "#%06XFF", color);
    return buffer;
}

std::string_view stripNamespace(std::string_view name) {
    const std::size_t separator = name.find(':');
    if (separator != std::string_view::npos) name.remove_prefix(separator + 1);
    return name;
}

bool contains(std::string_view value, std::string_view part) {
    return value.find(part) != std::string_view::npos;
}

void clearSourceCache() {
    g_cacheEpoch.fetch_add(1, std::memory_order_acq_rel);
    std::scoped_lock lock(g_sourceCacheMutex);
    g_sourceCache.clear();
}

void invalidateSourceNeighborhood(void* region, const BlockPosRaw& position) {
    if (!region) return;
    const int centerX = floorDiv16(position.x);
    const int centerY = floorDiv16(position.y);
    const int centerZ = floorDiv16(position.z);
    g_cacheEpoch.fetch_add(1, std::memory_order_acq_rel);
    std::scoped_lock lock(g_sourceCacheMutex);
    for (int y = centerY - 1; y <= centerY + 1; ++y) {
        for (int z = centerZ - 1; z <= centerZ + 1; ++z) {
            for (int x = centerX - 1; x <= centerX + 1; ++x) {
                g_sourceCache.erase({region, x, y, z});
            }
        }
    }
}

std::optional<Rgb> sourceColor(const void* rawBlock) {
    if (!rawBlock) return std::nullopt;
    const auto emission = bedrocktools::sdk::field<const std::uint8_t>(
        rawBlock,
        bedrocktools::sdk::offsets::Block::mLightEmission
    );
    if (emission == 0 || emission > 15) return std::nullopt;

    const auto* block = static_cast<const bedrocktools::sdk::Block*>(rawBlock);
    const std::string* fullName = block->fullName();
    if (!fullName || fullName->size() > 256) return std::nullopt;
    const std::string_view name = stripNamespace(*fullName);

    const auto load = [](const std::atomic<std::uint32_t>& color) {
        return unpackColor(color.load(std::memory_order_relaxed));
    };

    if (contains(name, "redstone_torch") || contains(name, "redstone_lamp")
        || contains(name, "redstone_ore")) {
        return load(g_redstoneColor);
    }
    if (contains(name, "soul_torch") || contains(name, "soul_lantern")
        || contains(name, "soul_campfire") || name == "soul_fire") {
        return load(g_soulColor);
    }
    if (name == "sea_lantern" || name == "conduit" || contains(name, "sea_pickle")) {
        return load(g_seaColor);
    }
    if (contains(name, "copper_torch") || contains(name, "copper_lantern")
        || contains(name, "verdant_froglight") || name == "glow_lichen"
        || contains(name, "cave_vines") || contains(name, "end_portal_frame")) {
        return load(g_greenColor);
    }
    if (contains(name, "pearlescent_froglight") || contains(name, "amethyst")
        || contains(name, "portal") || contains(name, "respawn_anchor")
        || contains(name, "sculk")) {
        return load(g_purpleColor);
    }
    if (name == "end_rod" || name == "beacon") {
        const Rgb cool = load(g_seaColor);
        return Rgb{0.76f + cool.r * 0.24f, 0.76f + cool.g * 0.24f, 0.76f + cool.b * 0.24f};
    }
    if (contains(name, "ochre_froglight") || contains(name, "lava") || contains(name, "magma")
        || name == "fire" || contains(name, "campfire") || contains(name, "torch")
        || contains(name, "lantern") || contains(name, "glowstone") || contains(name, "shroomlight")
        || contains(name, "jack_o_lantern") || contains(name, "lit_pumpkin")
        || contains(name, "candle") || contains(name, "furnace") || contains(name, "smoker")
        || contains(name, "blast_furnace") || contains(name, "copper_bulb")) {
        return load(g_warmColor);
    }
    if (name == "light_block") return std::nullopt;
    return load(g_warmColor);
}

std::vector<ColoredSource> scanSourceCell(const SourceCellKey& key) {
    std::vector<ColoredSource> sources;
    if (!key.region || !g_getBlock) return sources;
    const int minX = key.x * 16;
    const int minY = key.y * 16;
    const int minZ = key.z * 16;
    sources.reserve(8);

    for (int y = minY; y < minY + 16; ++y) {
        for (int z = minZ; z < minZ + 16; ++z) {
            for (int x = minX; x < minX + 16; ++x) {
                const BlockPosRaw position{x, y, z};
                const void* block = g_getBlock(key.region, &position, 0);
                const auto color = sourceColor(block);
                if (!color.has_value()) continue;
                const auto emission = bedrocktools::sdk::field<const std::uint8_t>(
                    block,
                    bedrocktools::sdk::offsets::Block::mLightEmission
                );
                sources.push_back({position, *color, static_cast<float>(emission)});
            }
        }
    }
    return sources;
}

void appendCellSources(const SourceCellKey& key, std::vector<ColoredSource>& destination) {
    if (g_cacheBypassUpdates.load(std::memory_order_acquire) > 0) {
        std::vector<ColoredSource> scanned = scanSourceCell(key);
        destination.insert(destination.end(), scanned.begin(), scanned.end());
        return;
    }

    {
        std::scoped_lock lock(g_sourceCacheMutex);
        const auto found = g_sourceCache.find(key);
        if (found != g_sourceCache.end()) {
            destination.insert(destination.end(), found->second.begin(), found->second.end());
            return;
        }
    }

    const std::uint64_t epoch = g_cacheEpoch.load(std::memory_order_acquire);
    std::vector<ColoredSource> scanned = scanSourceCell(key);
    destination.insert(destination.end(), scanned.begin(), scanned.end());

    std::scoped_lock lock(g_sourceCacheMutex);
    if (epoch != g_cacheEpoch.load(std::memory_order_relaxed)) return;
    if (g_sourceCache.size() >= 4096) g_sourceCache.clear();
    g_sourceCache.try_emplace(key, std::move(scanned));
}

void gatherNearbySources(void* region, const BlockPosRaw& position, std::vector<ColoredSource>& sources) {
    const float radiusScale = g_radiusScale.load(std::memory_order_relaxed);
    const int cellRadius = std::clamp(static_cast<int>(std::ceil(16.0f * radiusScale / 16.0f)), 1, 2);
    const int centerX = floorDiv16(position.x);
    const int centerY = floorDiv16(position.y);
    const int centerZ = floorDiv16(position.z);
    sources.reserve(48);
    for (int cy = centerY - cellRadius; cy <= centerY + cellRadius; ++cy) {
        for (int cz = centerZ - cellRadius; cz <= centerZ + cellRadius; ++cz) {
            for (int cx = centerX - cellRadius; cx <= centerX + cellRadius; ++cx) {
                appendCellSources({region, cx, cy, cz}, sources);
            }
        }
    }
}

float wallTransmission(void* region, const BlockPosRaw& target, const BlockPosRaw& source) {
    if (!g_wallAttenuation.load(std::memory_order_relaxed) || !g_isSolidBlockingBlock
        || samePosition(target, source)) {
        return 1.0f;
    }

    const float sourceX = static_cast<float>(source.x) + 0.5f;
    const float sourceY = static_cast<float>(source.y) + 0.5f;
    const float sourceZ = static_cast<float>(source.z) + 0.5f;
    float startX = std::clamp(sourceX, static_cast<float>(target.x), static_cast<float>(target.x + 1));
    float startY = std::clamp(sourceY, static_cast<float>(target.y), static_cast<float>(target.y + 1));
    float startZ = std::clamp(sourceZ, static_cast<float>(target.z), static_cast<float>(target.z + 1));
    const float deltaX = sourceX - startX;
    const float deltaY = sourceY - startY;
    const float deltaZ = sourceZ - startZ;
    const float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    if (distance <= 1.0f) return 1.0f;

    const float inverseDistance = 1.0f / distance;
    startX += deltaX * inverseDistance * 0.03f;
    startY += deltaY * inverseDistance * 0.03f;
    startZ += deltaZ * inverseDistance * 0.03f;
    const int steps = std::max(2, static_cast<int>(std::ceil(distance * 1.25f)));
    BlockPosRaw last = target;
    float transmission = 1.0f;

    for (int step = 1; step < steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const BlockPosRaw sample{
            static_cast<int>(std::floor(startX + (sourceX - startX) * t)),
            static_cast<int>(std::floor(startY + (sourceY - startY) * t)),
            static_cast<int>(std::floor(startZ + (sourceZ - startZ) * t)),
        };
        if (samePosition(sample, last) || samePosition(sample, target) || samePosition(sample, source)) continue;
        last = sample;
        if (g_isSolidBlockingBlock(region, &sample)) {
            transmission *= 0.3f;
            if (transmission < 0.1f) return 0.1f;
        }
    }
    return transmission;
}

TintSample calculateVertexTint(const TintContext& context, float x, float y, float z) {
    if (!context.sources) return {};

    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float totalWeight = 0.0f;
    float strongestSaturation = 0.0f;
    const float radiusScale = g_radiusScale.load(std::memory_order_relaxed);
    for (const PreparedSource& prepared : *context.sources) {
        const ColoredSource& source = prepared.source;
        const float centerX = static_cast<float>(source.position.x) + 0.5f;
        const float centerY = static_cast<float>(source.position.y) + 0.5f;
        const float centerZ = static_cast<float>(source.position.z) + 0.5f;
        const float dx = std::max(0.0f, std::abs(x - centerX) - 0.5f);
        const float dy = std::max(0.0f, std::abs(y - centerY) - 0.5f);
        const float dz = std::max(0.0f, std::abs(z - centerZ) - 0.5f);
        const float distance = (dx + dy + dz) / radiusScale;
        const float remainingLight = source.emission - distance;
        if (remainingLight <= 0.0f) continue;
        const float weight = (remainingLight / 15.0f) * prepared.transmission;
        if (weight <= 0.001f) continue;
        red += source.color.r * weight;
        green += source.color.g * weight;
        blue += source.color.b * weight;
        totalWeight += weight;
        strongestSaturation = std::max(
            strongestSaturation,
            (remainingLight / source.emission) * prepared.transmission
        );
    }

    if (totalWeight <= 0.001f) return {};
    return {
        {red / totalWeight, green / totalWeight, blue / totalWeight},
        std::clamp(
            strongestSaturation * g_colorStrength.load(std::memory_order_relaxed),
            0.0f,
            1.0f
        ),
        true,
    };
}

std::uint32_t tintPackedColor(std::uint32_t packed, const TintSample& tint) {
    if (!tint.active || tint.amount <= 0.001f) return packed;
    const float red = static_cast<float>(packed & 0xFFu);
    const float green = static_cast<float>((packed >> 8) & 0xFFu);
    const float blue = static_cast<float>((packed >> 16) & 0xFFu);
    const float peak = std::max({red, green, blue});
    const float inverseAmount = 1.0f - tint.amount;
    const auto channel = [inverseAmount, &tint, peak](float base, float hue) {
        return static_cast<std::uint32_t>(std::clamp(
            std::lround(base * inverseAmount + peak * hue * tint.amount),
            0l,
            255l
        ));
    };
    const std::uint32_t outRed = channel(red, tint.color.r);
    const std::uint32_t outGreen = channel(green, tint.color.g);
    const std::uint32_t outBlue = channel(blue, tint.color.b);
    return (packed & 0xFF000000u) | (outBlue << 16) | (outGreen << 8) | outRed;
}

class TintGuard {
public:
    TintGuard(void* blockTessellator, void* meshTessellator, const BlockPosRaw& position)
        : m_previous(g_tintContext) {
        if (!g_enabled.load(std::memory_order_relaxed) || !blockTessellator || !meshTessellator) return;
        void* region = bedrocktools::sdk::field<void*>(
            blockTessellator,
            bedrocktools::sdk::offsets::BlockTessellator::mRegion
        );
        if (!region) return;
        std::vector<ColoredSource> nearby;
        gatherNearbySources(region, position, nearby);
        m_sources.reserve(nearby.size());
        for (const ColoredSource& source : nearby) {
            m_sources.push_back({source, wallTransmission(region, position, source.position)});
        }
        g_tintContext = {meshTessellator, &m_sources, !m_sources.empty()};
    }

    ~TintGuard() {
        g_tintContext = m_previous;
    }

private:
    TintContext m_previous;
    std::vector<PreparedSource> m_sources;
};

bool tessellateBlockDetour(
    void* blockTessellator,
    void* meshTessellator,
    const void* block,
    const BlockPosRaw* position,
    std::uint64_t faces,
    const void* airAndSimpleBlocks
) {
    if (!g_tessellateBlockOriginal) return false;
    if (!position) {
        return g_tessellateBlockOriginal(
            blockTessellator,
            meshTessellator,
            block,
            position,
            faces,
            airAndSimpleBlocks
        );
    }
    TintGuard guard(blockTessellator, meshTessellator, *position);
    return g_tessellateBlockOriginal(
        blockTessellator,
        meshTessellator,
        block,
        position,
        faces,
        airAndSimpleBlocks
    );
}

void colorDetour(void* tessellator, float red, float green, float blue, float alpha) {
    if (!g_colorOriginal) return;
    g_colorOriginal(tessellator, red, green, blue, alpha);
}

void colorObjectDetour(void* tessellator, const ColorRaw* color) {
    if (!g_colorObjectOriginal || !color) return;
    g_colorObjectOriginal(tessellator, color);
}

void vertexDetour(void* tessellator, float x, float y, float z) {
    if (!g_vertexOriginal) return;
    if (!g_tintContext.active || g_tintContext.tessellator != tessellator) {
        g_vertexOriginal(tessellator, x, y, z);
        return;
    }
    const bool colorSet = bedrocktools::sdk::field<const std::uint8_t>(
        tessellator,
        bedrocktools::sdk::offsets::Tessellator::mNextColorSet
    ) != 0;
    if (!colorSet) {
        g_vertexOriginal(tessellator, x, y, z);
        return;
    }

    auto& packed = bedrocktools::sdk::field<std::uint32_t>(
        tessellator,
        bedrocktools::sdk::offsets::Tessellator::mNextColor
    );
    const std::uint32_t originalColor = packed;
    packed = tintPackedColor(originalColor, calculateVertexTint(g_tintContext, x, y, z));
    g_vertexOriginal(tessellator, x, y, z);
    packed = originalColor;
}

class TerrainBuildGuard {
public:
    TerrainBuildGuard() : m_previous(g_insideTerrainBuild) {
        g_insideTerrainBuild = true;
    }

    ~TerrainBuildGuard() {
        g_insideTerrainBuild = m_previous;
    }

private:
    bool m_previous;
};

bool useNewTessellationDetour(const void* block, bool onlyNew) {
    if (g_enabled.load(std::memory_order_relaxed) && g_insideTerrainBuild && !onlyNew) {
        return false;
    }
    return g_useNewTessellationOriginal
        ? g_useNewTessellationOriginal(block, onlyNew)
        : onlyNew;
}

void renderChunkBuilderBuildDetour(
    void* builder,
    void* geometry,
    bool transparentLeaves,
    std::int32_t lightingType,
    bool forExport,
    const void* lightingCapabilities
) {
    if (!g_renderChunkBuilderBuildOriginal) return;
    if (!g_enabled.load(std::memory_order_relaxed) || forExport) {
        g_renderChunkBuilderBuildOriginal(
            builder,
            geometry,
            transparentLeaves,
            lightingType,
            forExport,
            lightingCapabilities
        );
        return;
    }

    TerrainBuildGuard guard;
    g_renderChunkBuilderBuildOriginal(
        builder,
        geometry,
        transparentLeaves,
        lightingType,
        forExport,
        lightingCapabilities
    );
}

void brightnessChangedDetour(void* coordinator, void* source, const BlockPosRaw* position) {
    const bool refreshColoredLighting =
        g_enabled.load(std::memory_order_relaxed) && source && position;
    if (refreshColoredLighting) {
        g_cacheBypassUpdates.store(3, std::memory_order_release);
        invalidateSourceNeighborhood(source, *position);
    }
    if (g_brightnessOriginal) g_brightnessOriginal(coordinator, source, position);
    if (refreshColoredLighting) {
        invalidateSourceNeighborhood(source, *position);
        g_rebuildDelayUpdates.store(2, std::memory_order_release);
    }
}

template <typename Function>
bedrocktools::hooks::Handle installHook(
    bedrocktools::memory::SignatureId id,
    void* detour,
    Function* original
) {
    const std::uintptr_t address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;
    return bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        detour,
        reinterpret_cast<void**>(original)
    );
}

bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance || !g_setAllDirty) return false;
    void* levelRenderer = bedrocktools::sdk::field<void*>(
        clientInstance,
        bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer
    );
    if (!levelRenderer) return false;
    void* node = bedrocktools::sdk::field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );

    bool rebuilt = false;
    std::size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = bedrocktools::sdk::field<void*>(node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = bedrocktools::sdk::field<void*>(
            node,
            bedrocktools::sdk::offsets::HashNode::mValuePointer
        );
        if (coordinator) {
            g_setAllDirty(coordinator, true, false);
            rebuilt = true;
        }
        node = next;
    }
    return rebuilt;
}

void handleClientUpdate(void* clientInstance) {
    int bypassUpdates = g_cacheBypassUpdates.load(std::memory_order_acquire);
    while (bypassUpdates > 0 && !g_cacheBypassUpdates.compare_exchange_weak(
        bypassUpdates,
        bypassUpdates - 1,
        std::memory_order_acq_rel
    )) {
    }

    int delayUpdates = g_rebuildDelayUpdates.load(std::memory_order_acquire);
    if (delayUpdates < 0) return;
    if (delayUpdates > 0) {
        g_rebuildDelayUpdates.compare_exchange_strong(
            delayUpdates,
            delayUpdates - 1,
            std::memory_order_acq_rel
        );
        return;
    }

    clearSourceCache();
    if (rebuildRenderChunks(clientInstance)) {
        int expected = 0;
        g_rebuildDelayUpdates.compare_exchange_strong(
            expected,
            -1,
            std::memory_order_acq_rel
        );
    }
}

void removeHooks() {
    bedrocktools::hooks::remove(g_renderChunkBuilderBuildHook);
    g_renderChunkBuilderBuildHook = nullptr;
    bedrocktools::hooks::remove(g_brightnessHook);
    g_brightnessHook = nullptr;
    bedrocktools::hooks::remove(g_colorObjectHook);
    g_colorObjectHook = nullptr;
    bedrocktools::hooks::remove(g_vertexHook);
    g_vertexHook = nullptr;
    bedrocktools::hooks::remove(g_colorHook);
    g_colorHook = nullptr;
    bedrocktools::hooks::remove(g_tessellateBlockHook);
    g_tessellateBlockHook = nullptr;
    bedrocktools::hooks::remove(g_useNewTessellationHook);
    g_useNewTessellationHook = nullptr;
    g_tessellateBlockOriginal = nullptr;
    g_colorOriginal = nullptr;
    g_colorObjectOriginal = nullptr;
    g_vertexOriginal = nullptr;
    g_useNewTessellationOriginal = nullptr;
    g_renderChunkBuilderBuildOriginal = nullptr;
    g_brightnessOriginal = nullptr;
}

}

void applySettings(const Settings& input) {
    const float colorIntensity = std::clamp(input.colorIntensity, 0.0f, 1.0f);
    const float lightSpread = std::clamp(input.lightSpread, 0.5f, 1.5f);
    const std::string redstoneColor = normalizeColor(input.redstoneColor, 0xFF321F);
    const std::string soulColor = normalizeColor(input.soulColor, 0x438DFF);
    const std::string seaColor = normalizeColor(input.seaColor, 0x59D7FF);
    const std::string warmColor = normalizeColor(input.warmColor, 0xFFB45C);
    const std::string purpleColor = normalizeColor(input.purpleColor, 0xB970FF);
    const std::string greenColor = normalizeColor(input.greenColor, 0x72F29A);

    g_colorStrength.store(colorIntensity, std::memory_order_relaxed);
    g_radiusScale.store(lightSpread, std::memory_order_relaxed);
    g_wallAttenuation.store(!input.lightThroughWalls, std::memory_order_relaxed);
    g_redstoneColor.store(input.customizeColors ? parseColor(redstoneColor, 0xFF321F) : 0xFF321F, std::memory_order_relaxed);
    g_soulColor.store(input.customizeColors ? parseColor(soulColor, 0x438DFF) : 0x438DFF, std::memory_order_relaxed);
    g_seaColor.store(input.customizeColors ? parseColor(seaColor, 0x59D7FF) : 0x59D7FF, std::memory_order_relaxed);
    g_warmColor.store(input.customizeColors ? parseColor(warmColor, 0xFFB45C) : 0xFFB45C, std::memory_order_relaxed);
    g_purpleColor.store(input.customizeColors ? parseColor(purpleColor, 0xB970FF) : 0xB970FF, std::memory_order_relaxed);
    g_greenColor.store(input.customizeColors ? parseColor(greenColor, 0x72F29A) : 0x72F29A, std::memory_order_relaxed);
    clearSourceCache();
}

bool installHooks() {
    if (g_getBlock && g_isSolidBlockingBlock && g_setAllDirty
        && g_useNewTessellationHook && g_tessellateBlockHook
        && g_colorHook && g_colorObjectHook && g_vertexHook && g_brightnessHook
        && g_renderChunkBuilderBuildHook) {
        return true;
    }

    g_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::BlockSourceGetBlockForTessellation
    ));
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock
    ));
    g_setAllDirty = reinterpret_cast<SetAllDirtyFn>(bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty
    ));

    if (!g_useNewTessellationHook) {
        g_useNewTessellationHook = installHook(
            bedrocktools::memory::SignatureId::BlockTessellatorPipelineUseNewTessellation,
            reinterpret_cast<void*>(useNewTessellationDetour),
            &g_useNewTessellationOriginal
        );
    }
    if (!g_tessellateBlockHook) {
        g_tessellateBlockHook = installHook(
            bedrocktools::memory::SignatureId::BlockTessellatorTessellateBlockInWorld,
            reinterpret_cast<void*>(tessellateBlockDetour),
            &g_tessellateBlockOriginal
        );
    }
    if (!g_colorHook) {
        g_colorHook = installHook(
            bedrocktools::memory::SignatureId::TessellatorColor,
            reinterpret_cast<void*>(colorDetour),
            &g_colorOriginal
        );
    }
    if (!g_colorObjectHook) {
        g_colorObjectHook = installHook(
            bedrocktools::memory::SignatureId::TessellatorColorObject,
            reinterpret_cast<void*>(colorObjectDetour),
            &g_colorObjectOriginal
        );
    }
    if (!g_vertexHook) {
        g_vertexHook = installHook(
            bedrocktools::memory::SignatureId::TessellatorVertex,
            reinterpret_cast<void*>(vertexDetour),
            &g_vertexOriginal
        );
    }
    if (!g_brightnessHook) {
        g_brightnessHook = installHook(
            bedrocktools::memory::SignatureId::RenderChunkCoordinatorOnBrightnessChanged,
            reinterpret_cast<void*>(brightnessChangedDetour),
            &g_brightnessOriginal
        );
    }
    if (!g_renderChunkBuilderBuildHook) {
        g_renderChunkBuilderBuildHook = installHook(
            bedrocktools::memory::SignatureId::RenderChunkBuilderBuild,
            reinterpret_cast<void*>(renderChunkBuilderBuildDetour),
            &g_renderChunkBuilderBuildOriginal
        );
    }
    const bool installed = g_getBlock && g_isSolidBlockingBlock && g_setAllDirty
        && g_useNewTessellationHook && g_tessellateBlockHook
        && g_colorHook && g_colorObjectHook && g_vertexHook && g_brightnessHook
        && g_renderChunkBuilderBuildHook;
    if (!installed) removeHooks();
    return installed;
}

bool install(const Settings& settings, bool active) {
    applySettings(settings);
    if (!installHooks()) return false;
    g_enabled.store(active, std::memory_order_release);
    g_rebuildDelayUpdates.store(0, std::memory_order_release);
    return true;
}

void setActive(bool active) {
    g_enabled.store(active, std::memory_order_release);
    clearSourceCache();
    g_cacheBypassUpdates.store(0, std::memory_order_release);
    g_rebuildDelayUpdates.store(0, std::memory_order_release);
}

void updateSettings(const Settings& settings) {
    applySettings(settings);
    if (g_enabled.load(std::memory_order_acquire)) {
        g_rebuildDelayUpdates.store(0, std::memory_order_release);
    }
}

void onClientInstanceUpdate(void* clientInstance) {
    if (!clientInstance || !g_enabled.load(std::memory_order_acquire)) return;
    g_lastClientInstance.store(clientInstance, std::memory_order_release);
    handleClientUpdate(clientInstance);
}

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    clearSourceCache();
    g_cacheBypassUpdates.store(0, std::memory_order_release);
    rebuildRenderChunks(g_lastClientInstance.load(std::memory_order_acquire));
    removeHooks();
    g_lastClientInstance.store(nullptr, std::memory_order_release);
}

}
