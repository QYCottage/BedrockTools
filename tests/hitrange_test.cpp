// Integration test for the Hit Range tick sampler, reach verdict and ring
// renderer using fake game memory. No Minecraft or Android runtime required.
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "fake_shader_color.hpp"

namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId) { return 0; } }
#include "modules/visual/hitrange.cpp"

namespace {
using namespace bedrocktools::sdk;
namespace off = bedrocktools::sdk::offsets;

int vertices = 0, begins = 0, flushes = 0, lastCount = 0;
std::vector<std::array<float, 4>> colors;
std::vector<void*> materials;
fake_render::GpuColor gpu;

std::array<std::byte, 0x100> g_level{};
std::array<std::byte, 0x100> g_hitResult{};
std::vector<DistanceSortedActor> g_nearby;

void* fakeLevelGetHitResult(void*) { return g_hitResult.data(); }
ActorVec fakeFetch(void*, void*, int) {
    return ActorVec{g_nearby.data(), g_nearby.data() + g_nearby.size(),
                    g_nearby.data() + g_nearby.size()};
}
void fakeBegin(void*, void*, int mode, int count, int) {
    assert(mode == 1);
    lastCount = count;
    ++begins;
}
void fakeColor(void*, float r, float g, float b, float a) {
    colors.push_back({r, g, b, a});
}
void fakeVertex(void*, float, float, float) { ++vertices; }
void fakeFlush(void* screen, void*, void* material, char*) {
    ++flushes;
    materials.push_back(material);
    auto* holder = fake_render::colorHolder(screen);
    assert(holder);
    gpu.upload(*holder);
}
void resetDraw() {
    vertices = begins = flushes = lastCount = 0;
    colors.clear();
    materials.clear();
}

template <typename T, std::size_t N>
void put(std::array<std::byte, N>& data, std::size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

struct FakeActor {
    alignas(std::max_align_t) std::array<std::byte, 0x500> data{};
    alignas(std::max_align_t) std::array<std::byte, 0x100> component{};

    void set(const AABB& box, std::uint32_t categories, void* level = nullptr) {
        put(data, off::Actor::mCategories, categories);
        put(data, off::Actor::mStateVectorComponent + off::BuiltInActorComponents::mAABBShapeComponent,
            reinterpret_cast<std::uintptr_t>(component.data()));
        put(component, off::AABBShapeComponent::mAABB, box);
        if (level) put(data, off::Actor::mLevel, reinterpret_cast<std::uintptr_t>(level));
    }
    void* ptr() { return data.data(); }
};

void expectColor(std::size_t index, std::uint32_t value) {
    assert(index < colors.size());
    const float r = ((value >> 16) & 255) / 255.0f;
    const float g = ((value >> 8) & 255) / 255.0f;
    const float b = (value & 255) / 255.0f;
    assert(std::abs(colors[index][0] - r) < 0.001f);
    assert(std::abs(colors[index][1] - g) < 0.001f);
    assert(std::abs(colors[index][2] - b) < 0.001f);
}
} // namespace

int main() {
    using namespace hitrange;
    const AABB playerBox{{-0.3f, 64.0f, -0.3f}, {0.3f, 65.8f, 0.3f}};
    const AABB nearBox{{2.0f, 64.0f, -0.3f}, {2.6f, 65.8f, 0.3f}};
    const AABB farPlayerBox{{4.7f, 64.0f, -0.3f}, {5.3f, 65.8f, 0.3f}};
    const AABB farMobBox{{6.7f, 64.0f, -0.3f}, {7.3f, 65.8f, 0.3f}};

    FakeActor player, targetPlayer, targetMob;
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);
    put(g_hitResult, off::HitResult::mType, off::HitResult::TypeNoHit);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}, {targetMob.ptr(), 7.0f, 0.0f}};

    alignas(std::max_align_t) std::array<std::byte, 0x200> screen{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> renderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> camera{};
    fake_render::ShaderColor color{{.2f, .3f, .4f, .5f}, 0};
    fake_render::ShaderColor legacy{{.7f, .6f, .5f, .4f}, 0};
    put(screen, off::ScreenContext::mTessellator, reinterpret_cast<std::uintptr_t>(&vertices));
    put(screen, fake_render::kColorHolderOffset, reinterpret_cast<std::uintptr_t>(&color));
    put(screen, fake_render::kOldColorHolderOffset, reinterpret_cast<std::uintptr_t>(&legacy));
    put(renderer, off::LevelRenderer::mLevelRendererPlayer,
        reinterpret_cast<std::uintptr_t>(camera.data()));
    put(camera, off::LevelRendererPlayer::mCamPos, Vec3{0, 65, 0});
    gpu.rgba = {0, 0, 0, 1};

    HitRangeModule mod;
    mod.onInit();
    mod.setMasterEnabled(true);
    tessBegin = fakeBegin;
    tessColor = fakeColor;
    tessVertex = fakeVertex;
    renderMesh = fakeFlush;
    levelGetHitResult = fakeLevelGetHitResult;
    fetchNearby = fakeFetch;
    materialGroup = 1;
    depthMaterial.data[0] = &vertices;
    throughMaterial.data[0] = &vertices;
    mod.showMobs = true;

    tick(player.ptr());
    assert(state.hasPlayer && state.targetPresent && state.inRange);
    assert(state.targets.size() == 2);
    assert(state.targets[0].inRange && !state.targets[1].inRange);

    render(renderer.data(), screen.data());
    assert(begins == 1 && flushes == 1);
    assert(lastCount == 3 * vertexCount && vertices == lastCount);
    assert(colors.size() == static_cast<std::size_t>(vertices));
    expectColor(0, mod.inRangeColor);                 // own ring: green
    expectColor(vertexCount, mod.inRangeColor);       // nearby player: green
    expectColor(2 * vertexCount, mod.outOfRangeColor); // distant mob: red
    assert(materials.back() == &depthMaterial);
    assert((gpu.rgba == fake_render::Color{1, 1, 1, 1}));
    assert((color.rgba == fake_render::Color{.2f, .3f, .4f, .5f}) && color.dirty == 1);
    assert((legacy.rgba == fake_render::Color{.7f, .6f, .5f, .4f}));

    // Move every target outside the 3-block reach: all three rings turn red.
    targetPlayer.set(farPlayerBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);
    tick(player.ptr());
    assert(state.targetPresent && !state.inRange);
    assert(!state.targets[0].inRange && !state.targets[1].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    assert(lastCount == 3 * vertexCount);
    expectColor(0, mod.outOfRangeColor);
    expectColor(vertexCount, mod.outOfRangeColor);
    expectColor(2 * vertexCount, mod.outOfRangeColor);

    // Without the actor fetch, the game's own crosshair verdict still decides.
    fetchNearby = nullptr;
    put(g_hitResult, off::HitResult::mType, off::HitResult::TypeEntity);
    tick(player.ptr());
    assert(state.targets.empty() && state.targetPresent && state.inRange);
    put(g_hitResult, off::HitResult::mType, off::HitResult::TypeEntityOutOfRange);
    tick(player.ptr());
    assert(state.targetPresent && !state.inRange);
    put(g_hitResult, off::HitResult::mType, off::HitResult::TypeNoHit);
    tick(player.ptr());
    assert(!state.targetPresent && !state.inRange);

    fetchNearby = fakeFetch;
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);

    // Group toggles decide both the target rings and the color verdict.
    mod.showMobs = false;
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.inRange);
    mod.showPlayers = false;
    mod.showMobs = true;
    tick(player.ptr());
    assert(state.targets.size() == 1 && !state.inRange);
    mod.showPlayers = true;
    mod.showMobs = false;
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.inRange);

    // Show toggles affect only the draw list.
    resetDraw();
    mod.showSelf = false;
    render(renderer.data(), screen.data());
    assert(lastCount == static_cast<int>(state.targets.size()) * vertexCount);
    resetDraw();
    mod.showTargets = false;
    mod.showSelf = true;
    render(renderer.data(), screen.data());
    assert(lastCount == vertexCount && begins == 1);
    resetDraw();
    mod.showSelf = false;
    render(renderer.data(), screen.data());
    assert(begins == 0 && flushes == 0 && lastCount == 0);
    mod.showSelf = true;
    mod.showTargets = true;

    // Through Walls selects the no-depth material when it is available.
    resetDraw();
    mod.seeThrough = true;
    render(renderer.data(), screen.data());
    assert(materials.back() == &throughMaterial);
    resetDraw();
    mod.seeThrough = false;
    render(renderer.data(), screen.data());
    assert(materials.back() == &depthMaterial);

    // Config round trip, including both color syntaxes and malformed values.
    nlohmann::json settings;
    settings["reach"] = 4.5f;
    settings["lineWidth"] = 0.2f;
    settings["effectStyle"] = "2";
    settings["inRangeColor"] = "#80123456";
    settings["outOfRangeColor"] = "#ABCDEF";
    settings["showSelf"] = false;
    settings["showTargets"] = false;
    settings["showPlayers"] = false;
    settings["showMobs"] = true;
    settings["seeThrough"] = true;
    mod.loadConfig(settings);
    assert(mod.reach == 4.5f && mod.lineWidth == 0.2f && mod.style == 2);
    assert(mod.inRangeColor == 0x80123456u && mod.outOfRangeColor == 0xFFABCDEFu);
    assert(!mod.showSelf && !mod.showTargets && !mod.showPlayers && mod.showMobs && mod.seeThrough);
    nlohmann::json invalid;
    invalid["inRangeColor"] = "#not-a-color";
    mod.loadConfig(invalid);
    assert(mod.inRangeColor == 0x80123456u);
    nlohmann::json saved;
    mod.saveConfig(saved);
    assert(saved["reach"].get<float>() == 4.5f);
    assert(saved["lineWidth"].get<float>() == 0.2f);
    assert(saved["inRangeColor"].get<std::string>() == "#80123456");
    assert(saved["outOfRangeColor"].get<std::string>() == "#FFABCDEF");
    assert(saved["effectStyle"].get<std::string>() == jumpcircle::styleRadioValue(2));
    assert(!saved["showSelf"].get<bool>() && saved["seeThrough"].get<bool>());
    mod.loadConfig(saved);
    assert(mod.reach == 4.5f && mod.style == 2 && mod.inRangeColor == 0x80123456u);
    for (int i = 0; i < jumpcircle::styleCount; ++i) {
        nlohmann::json choice;
        choice["effectStyle"] = std::to_string(i);
        mod.loadConfig(choice);
        assert(mod.style == i);
        nlohmann::json roundTrip;
        mod.saveConfig(roundTrip);
        assert(jumpcircle::styleIndex(roundTrip["effectStyle"].get<std::string>()) == i);
    }
    assert(safeReach(saved["reach"].get<float>()) == 4.5f);
    assert(safeWidth(saved["lineWidth"].get<float>()) == 0.2f);

    // A stale snapshot (pause / world change) draws nothing; disable clears it.
    resetDraw();
    {
        std::lock_guard lock(stateMutex);
        state.lastTick = Clock::now() - std::chrono::seconds(1);
    }
    render(renderer.data(), screen.data());
    assert(begins == 0 && flushes == 0);
    mod.onDisable();
    assert(!state.hasPlayer && state.targets.empty() && trackedPlayer == nullptr);
    return 0;
}
