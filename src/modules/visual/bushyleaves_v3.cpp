#include "bushyleaves.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

struct Vec3Raw { float x, y, z; };

enum class LeafFace : uint8_t { Down = 0, Up, North, South, West, East };

using FaceFn = void (*)(void*, void*, const void*, const Vec3Raw*, const void*);

struct FaceHook {
    bedrocktools::hooks::Handle handle = nullptr;
    FaceFn original = nullptr;
};

std::atomic_bool g_enabled{false};
std::atomic_int g_layers{3};
std::atomic<float> g_spread{0.08f};
std::atomic_bool g_affectTopBottom{true};
std::atomic_bool g_affectSides{true};

FaceHook g_faceHooks[6] = {};

std::string_view stripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

bool isLeaf(const void* block) {
    if (!block) return false;

    const auto* blockObj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* fullName = blockObj->fullName();
    if (!fullName || fullName->empty() || fullName->size() > 256) return false;

    const std::string_view name = stripNamespace({fullName->data(), fullName->size()});

    return name == "oak_leaves"
        || name == "spruce_leaves"
        || name == "birch_leaves"
        || name == "jungle_leaves"
        || name == "acacia_leaves"
        || name == "dark_oak_leaves"
        || name == "mangrove_leaves"
        || name == "cherry_leaves"
        || name == "pale_oak_leaves"
        || name == "azalea_leaves"
        || name == "flowering_azalea_leaves"
        || name.ends_with("_leaves");
}

bool faceEnabled(LeafFace face) {
    if (face == LeafFace::Up || face == LeafFace::Down)
        return g_affectTopBottom.load(std::memory_order_relaxed);
    return g_affectSides.load(std::memory_order_relaxed);
}

void renderBushyFace(
    FaceFn original,
    LeafFace face,
    void* tessellator,
    void* meshTessellator,
    const void* block,
    const Vec3Raw* position,
    const void* texture
) {
    if (!original) return;

    // Always keep the original vanilla face.
    original(tessellator, meshTessellator, block, position, texture);

    if (!g_enabled.load(std::memory_order_relaxed)
        || !tessellator || !block || !position || !texture
        || !isLeaf(block)
        || !faceEnabled(face)) {
        return;
    }

    const int layers = std::clamp(g_layers.load(std::memory_order_relaxed), 1, 6);
    const float spread = std::clamp(g_spread.load(std::memory_order_relaxed), 0.01f, 0.20f);

    // Tangent directions for the current face. Copies are moved mainly along
    // the face plane, producing a soft/irregular foliage silhouette instead
    // of changing the block's collision or texture.
    Vec3Raw a{}, b{}, n{};
    switch (face) {
    case LeafFace::Down:
        a = {1.0f, 0.0f, 0.0f}; b = {0.0f, 0.0f, 1.0f}; n = {0.0f, -1.0f, 0.0f}; break;
    case LeafFace::Up:
        a = {1.0f, 0.0f, 0.0f}; b = {0.0f, 0.0f, 1.0f}; n = {0.0f, 1.0f, 0.0f}; break;
    case LeafFace::North:
        a = {1.0f, 0.0f, 0.0f}; b = {0.0f, 1.0f, 0.0f}; n = {0.0f, 0.0f, -1.0f}; break;
    case LeafFace::South:
        a = {1.0f, 0.0f, 0.0f}; b = {0.0f, 1.0f, 0.0f}; n = {0.0f, 0.0f, 1.0f}; break;
    case LeafFace::West:
        a = {0.0f, 0.0f, 1.0f}; b = {0.0f, 1.0f, 0.0f}; n = {-1.0f, 0.0f, 0.0f}; break;
    case LeafFace::East:
        a = {0.0f, 0.0f, 1.0f}; b = {0.0f, 1.0f, 0.0f}; n = {1.0f, 0.0f, 0.0f}; break;
    }

    for (int layer = 1; layer <= layers; ++layer) {
        const float d = spread * static_cast<float>(layer) / static_cast<float>(layers);
        const float normalOffset = d * 0.18f;

        // Four offset copies spread the leaf pixels around the face.
        const std::array<Vec3Raw, 4> offsets = {{
            { a.x * d + n.x * normalOffset,  a.y * d + n.y * normalOffset,  a.z * d + n.z * normalOffset },
            {-a.x * d + n.x * normalOffset, -a.y * d + n.y * normalOffset, -a.z * d + n.z * normalOffset },
            { b.x * d - n.x * normalOffset,  b.y * d - n.y * normalOffset,  b.z * d - n.z * normalOffset },
            {-b.x * d - n.x * normalOffset, -b.y * d - n.y * normalOffset, -b.z * d - n.z * normalOffset }
        }};

        for (const Vec3Raw& o : offsets) {
            const Vec3Raw p{
                position->x + o.x,
                position->y + o.y,
                position->z + o.z
            };
            original(tessellator, meshTessellator, block, &p, texture);
        }
    }
}

template <LeafFace Face>
void faceHookTrampoline(
    void* a0,
    void* a1,
    const void* a2,
    const Vec3Raw* a3,
    const void* a4
) {
    renderBushyFace(
        g_faceHooks[static_cast<size_t>(Face)].original,
        Face,
        a0, a1, a2, a3, a4
    );
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
    &faceHookTrampoline<LeafFace::Down>,
    &faceHookTrampoline<LeafFace::Up>,
    &faceHookTrampoline<LeafFace::North>,
    &faceHookTrampoline<LeafFace::South>,
    &faceHookTrampoline<LeafFace::West>,
    &faceHookTrampoline<LeafFace::East>,
};

template <typename Fn>
bedrocktools::hooks::Handle installHook(
    bedrocktools::memory::SignatureId id,
    void* detour,
    Fn* original
) {
    const uintptr_t address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;

    return bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        detour,
        reinterpret_cast<void**>(original)
    );
}

} // namespace

BushyLeavesModule::BushyLeavesModule()
    : Module(
        "Bushy Leaves",
        "Adds fluffy layered geometry to vanilla leaf blocks."
    ) {
}

BushyLeavesModule::~BushyLeavesModule() {
    g_enabled.store(false, std::memory_order_relaxed);
}

void BushyLeavesModule::applySettings() {
    layers = std::clamp(layers, 1, 6);
    spread = std::clamp(spread, 0.01f, 0.20f);

    g_layers.store(layers, std::memory_order_relaxed);
    g_spread.store(spread, std::memory_order_relaxed);
    g_affectTopBottom.store(affectTopBottom, std::memory_order_relaxed);
    g_affectSides.store(affectSides, std::memory_order_relaxed);
}

void BushyLeavesModule::installHooks() {
    if (m_hooked) return;

    bool allHooked = true;

    for (size_t i = 0; i < 6; ++i) {
        if (!g_faceHooks[i].handle) {
            g_faceHooks[i].handle = installHook(
                kFaceSigIds[i],
                reinterpret_cast<void*>(kFaceTrampolines[i]),
                &g_faceHooks[i].original
            );
        }

        if (!g_faceHooks[i].handle || !g_faceHooks[i].original)
            allHooked = false;
    }

    m_hooked = allHooked;
}

void BushyLeavesModule::onInit() {
    applySettings();
    installHooks();
}

void BushyLeavesModule::onEnable() {
    applySettings();

    if (!m_hooked)
        installHooks();

    g_enabled.store(m_hooked, std::memory_order_release);
}

void BushyLeavesModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
}

void BushyLeavesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    layers = j.value("layers", layers);
    spread = j.value("spread", spread);
    affectTopBottom = j.value("affectTopBottom", affectTopBottom);
    affectSides = j.value("affectSides", affectSides);

    applySettings();
}

void BushyLeavesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["layers"] = layers;
    j["spread"] = spread;
    j["affectTopBottom"] = affectTopBottom;
    j["affectSides"] = affectSides;
}
