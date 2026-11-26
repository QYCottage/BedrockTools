// Integration test for the Hit Range tick sampler, camera-independent range/LOS
// verdict, stable attack-frame rings, auto-detected survival/creative reach and
// ring renderer using fake game memory.
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
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

alignas(std::max_align_t) std::array<std::byte, 0x100> g_level{};
alignas(std::max_align_t) std::array<std::byte, 0x100> g_dimension{};
alignas(std::max_align_t) std::array<std::byte, 0x100> g_region{};
enum class SightMode { Clear, Wall, LowWall, Above, Below };
SightMode g_sightMode = SightMode::Clear;
std::vector<DistanceSortedActor> g_nearby;

ActorVec fakeFetch(void*, void*, int) {
    return ActorVec{g_nearby.data(), g_nearby.data() + g_nearby.size(),
                    g_nearby.data() + g_nearby.size()};
}
bool fakeIsSolid(void* region, const BlockPosI& position) {
    assert(region == g_region.data());
    switch (g_sightMode) {
        case SightMode::Wall: return true;
        case SightMode::LowWall: return position.y == 64 && position.z == 1;
        case SightMode::Above: return position.y == 65 && position.z == 2;
        case SightMode::Below: return position.y == 66 && position.z == 1;
        case SightMode::Clear: return false;
    }
    return false;
}
void fakeBegin(void*, void*, int mode, int count, int) {
    assert(mode == 1);
    lastCount = count;
    ++begins;
}
void fakeColor(void*, float r, float g, float b, float a) {
    colors.push_back({r, g, b, a});
}
void fakeVertex(void*, float x, float y, float z) {
    (void)y;
    assert(std::abs(x) <= 16.0f && std::abs(z) <= 16.0f);
    ++vertices;
}
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
void fresh(SightMode mode = SightMode::Clear) {
    clearState();
    resetDraw();
    g_sightMode = mode;
}

template <typename T, std::size_t N>
void put(std::array<std::byte, N>& data, std::size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

struct FakeActor {
    // Room for the 1.26.60 Player::mGameMode field and the bounded scan.
    alignas(std::max_align_t) std::array<std::byte, 0xC00> data{};
    alignas(std::max_align_t) std::array<std::byte, 0x100> component{};
    alignas(std::max_align_t) std::array<std::byte, 0x40> rotation{};

    void set(const AABB& box, std::uint32_t categories, void* level = nullptr,
             Vec2 rot = Vec2{0, 0}) {
        put(data, off::Actor::mCategories, categories);
        put(data, off::Actor::mStateVectorComponent + off::BuiltInActorComponents::mAABBShapeComponent,
            reinterpret_cast<std::uintptr_t>(component.data()));
        put(component, off::AABBShapeComponent::mAABB, box);
        put(data, off::Actor::mActorRotationComponent,
            reinterpret_cast<std::uintptr_t>(rotation.data()));
        put(rotation, 0, rot);
        if (level) put(data, off::Actor::mLevel, reinterpret_cast<std::uintptr_t>(level));
        put(data, off::Actor::mDimension, reinterpret_cast<std::uintptr_t>(g_dimension.data()));
        put(g_dimension, off::Dimension::mBlockSource,
            reinterpret_cast<std::uintptr_t>(g_region.data()));
    }
    void setGameMode(void* gameMode) {
        put(data, off::Player::mGameMode, reinterpret_cast<std::uintptr_t>(gameMode));
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
    const AABB frontBox{{-0.3f, 64.0f, 2.0f}, {0.3f, 65.8f, 2.6f}};
    const AABB behindBox{{-0.3f, 64.0f, -2.6f}, {0.3f, 65.8f, -2.0f}};
    const AABB offCrosshairBox{{1.5f, 64.0f, 1.5f}, {2.1f, 65.8f, 2.1f}};
    const AABB highPlayerBox{{-0.3f, 65.0f, -0.3f}, {0.3f, 66.8f, 0.3f}};
    const AABB lowPlayerBox{{-0.3f, 63.0f, -0.3f}, {0.3f, 64.8f, 0.3f}};
    const AABB elevatedBox{{-0.3f, 65.0f, 2.0f}, {0.3f, 66.8f, 2.6f}};

    FakeActor player, targetPlayer, targetMob, distantMob;
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    targetMob.set(offCrosshairBox, off::ActorCategories::IsMob);
    distantMob.set(farMobBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f},
                {targetMob.ptr(), 2.4f, 0.0f},
                {distantMob.ptr(), 7.0f, 0.0f}};

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
    mod.setCreativeMode(false);
    tessBegin = fakeBegin;
    tessColor = fakeColor;
    tessVertex = fakeVertex;
    renderMesh = fakeFlush;
    fetchNearby = fakeFetch;
    isSolidBlockingBlock = fakeIsSolid;
    materialGroup = 1;
    depthMaterial.data[0] = &vertices;
    throughMaterial.data[0] = &vertices;
    mod.showMobs = true;

    // Problem 5: green is determined only by eye distance and native LOS.
    // No crosshair HitResult or camera direction is sampled by the module.
    fresh();
    tick(player.ptr());
    assert(state.hasPlayer);
    assert(state.hittableActor == targetPlayer.ptr());
    assert(state.targetPresent && state.inRange && state.reach == survivalReach);
    assert(state.targets.size() == 3);
    assert(state.targets[0].inRange && state.targets[1].inRange &&
           !state.targets[2].inRange);
    assert(mod.effectiveReach() == survivalReach);

    render(renderer.data(), screen.data());
    assert(begins == 1 && flushes == 1);
    assert(lastCount == 4 * vertexCount && vertices == lastCount);
    assert(colors.size() == static_cast<std::size_t>(vertices));
    expectColor(0, mod.inRangeColor);                  // own ring: green
    expectColor(vertexCount, mod.inRangeColor);        // front player: green
    expectColor(2 * vertexCount, mod.inRangeColor);    // off-axis mob: green
    expectColor(3 * vertexCount, mod.outOfRangeColor); // distant mob: red
    assert(materials.back() == &depthMaterial);
    assert((gpu.rgba == fake_render::Color{1, 1, 1, 1}));
    assert((color.rgba == fake_render::Color{.2f, .3f, .4f, .5f}) && color.dirty == 1);
    assert((legacy.rgba == fake_render::Color{.7f, .6f, .5f, .4f}));

    // A 180-degree screen rotation must not change any ring color or radius.
    fresh();
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data(), Vec2{0, 180});
    tick(player.ptr());
    assert(state.targetPresent && state.inRange && state.reach == survivalReach);
    assert(state.targets.size() == 3);
    assert(state.targets[0].inRange && state.targets[1].inRange &&
           !state.targets[2].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);
    expectColor(2 * vertexCount, mod.inRangeColor);
    expectColor(3 * vertexCount, mod.outOfRangeColor);
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data());

    // Problem 6: a target one block above/below the eye stays green when the
    // 3D distance is inside reach and the nearest top/bottom face is clear.
    fresh(SightMode::Above);
    player.set(highPlayerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    assert(state.targetPresent && state.inRange && state.targets.size() == 1);
    assert(state.targets[0].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);

    fresh(SightMode::Below);
    player.set(lowPlayerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(elevatedBox, off::ActorCategories::IsPlayer);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    assert(state.targetPresent && state.inRange && state.targets.size() == 1);
    assert(state.targets[0].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);

    // A full wall still rejects the same vertical angles.
    fresh(SightMode::Wall);
    tick(player.ptr());
    assert(!state.inRange && !state.targets[0].inRange);

    // Restore the normal-level scenario for the remaining regressions.
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    targetMob.set(offCrosshairBox, off::ActorCategories::IsMob);
    distantMob.set(farMobBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f},
                {targetMob.ptr(), 2.4f, 0.0f},
                {distantMob.ptr(), 7.0f, 0.0f}};

    // A solid wall vetoes every target: no behind-wall false green.
    fresh(SightMode::Wall);
    tick(player.ptr());
    assert(state.targetPresent && !state.inRange);
    assert(!state.targets[0].inRange && !state.targets[1].inRange &&
           !state.targets[2].inRange);
    render(renderer.data(), screen.data());
    expectColor(0, mod.outOfRangeColor);
    expectColor(vertexCount, mod.outOfRangeColor);

    // A partially visible target (head line clear, body line blocked) must
    // stay red: problem 5 showed that a permissive green could fail to hit.
    fresh(SightMode::LowWall);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}}; // frontBox: center line blocked
    tick(player.ptr());
    assert(state.targetPresent && !state.inRange);
    assert(state.targets.size() == 1 && !state.targets[0].inRange);

    // A target beside/behind the player is eligible when inside reach with a
    // clear sight line; plain AABB proximity alone still cannot beat a wall.
    fresh();
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    targetMob.set(behindBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}, {targetMob.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    assert(state.targetPresent && state.inRange);
    assert(state.targets[0].inRange && state.targets[1].inRange);
    fresh(SightMode::Wall);
    tick(player.ptr());
    assert(!state.inRange && !state.targets[0].inRange);

    // Without the native solid-block signature, fail closed to red. There is
    // no exact TypeEntity/crosshair fallback that can promise a false hit.
    fresh();
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}};
    isSolidBlockingBlock = nullptr;
    tick(player.ptr());
    assert(!state.inRange && !state.targets[0].inRange);
    isSolidBlockingBlock = fakeIsSolid;

    // A confirmed attack cannot flash a false green or resize the rings from
    // 3 to 5 blocks. A solid wall is an authoritative range/LOS red verdict.
    fresh(SightMode::Wall);
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    bedrocktools::events::AttackEvent badHint{
        bedrocktools::events::AttackKind::GameMode, nullptr,
        reinterpret_cast<bedrocktools::sdk::Actor*>(targetPlayer.ptr()), nullptr, nullptr};
    bedrocktools::events::bus().publish(badHint);
    tick(player.ptr());
    assert(!state.inRange && !state.targets[0].inRange);
    assert(mod.effectiveReach() == survivalReach);

    // A hit-frame actor-fetch miss keeps the last stable target ring and color.
    fresh();
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    assert(state.inRange && state.targets.size() == 1);
    bedrocktools::events::AttackEvent hit{
        bedrocktools::events::AttackKind::SurvivalMode, nullptr,
        reinterpret_cast<bedrocktools::sdk::Actor*>(targetPlayer.ptr()), nullptr, nullptr};
    bedrocktools::events::bus().publish(hit);
    g_nearby.clear();
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.targets[0].held);
    assert(state.inRange && state.targets[0].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    assert(lastCount == 2 * vertexCount);
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);

    // Expire the hold: the stale actor ring is dropped without dereferencing it.
    {
        std::lock_guard lock(stateMutex);
        state.targets[0].lastSeen = Clock::now() - targetHoldWindow - std::chrono::milliseconds(1);
        state.verdictHold = {};
    }
    {
        std::lock_guard attackLock(recentAttackMutex);
        recentAttack.at = Clock::now() - targetHoldWindow - std::chrono::milliseconds(1);
    }
    tick(player.ptr());
    assert(state.targets.empty());

    // Knockback/recycled reads cannot teleport a target ring centre.
    fresh();
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    const float beforeX = state.targets[0].stableCenter.x;
    targetPlayer.set(farPlayerBox, off::ActorCategories::IsPlayer);
    tick(player.ptr());
    const float afterX = state.targets[0].stableCenter.x;
    assert(afterX - beforeX <= maxCenterStep + 0.0001f);
    assert(afterX > beforeX);

    // Group toggles change which rings are drawn, not the range/LOS decision.
    fresh();
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}, {targetMob.ptr(), 7.0f, 0.0f}};
    mod.showMobs = false;
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.inRange);
    fresh();
    mod.showPlayers = false;
    mod.showMobs = true;
    tick(player.ptr());
    assert(state.targets.size() == 1 && !state.inRange);
    fresh();
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

    // Reach is detected, never configured. Weak event hints are debounced and
    // attack events are ignored completely.
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    survivalAttackFn = creativeAttackFn = creativeInternalFn = 0;
    learnedSurvivalVTable.store(nullptr);
    learnedCreativeVTable.store(nullptr);
    assert(mod.effectiveReach() == survivalReach);
    bedrocktools::events::AttackEvent ignoredCreative{
        bedrocktools::events::AttackKind::GameMode, nullptr, nullptr, nullptr, nullptr};
    bedrocktools::events::bus().publish(ignoredCreative);
    assert(mod.effectiveReach() == survivalReach);
    bedrocktools::events::GameModeActionEvent weakCreative{
        bedrocktools::events::GameModeType::GameMode,
        bedrocktools::events::GameModeAction::StartBuildBlock, nullptr};
    bedrocktools::events::bus().publish(weakCreative);
    assert(mod.effectiveReach() == survivalReach);
    bedrocktools::events::bus().publish(weakCreative);
    assert(mod.effectiveReach() == creativeReach);
    bedrocktools::events::GameModeActionEvent weakSurvival{
        bedrocktools::events::GameModeType::SurvivalMode,
        bedrocktools::events::GameModeAction::StartBuildBlock, nullptr};
    bedrocktools::events::bus().publish(weakSurvival);
    assert(mod.effectiveReach() == creativeReach);
    bedrocktools::events::bus().publish(weakSurvival);
    assert(mod.effectiveReach() == survivalReach);
    assert(modeFromGameMode(nullptr) == ModeHint::Unknown);

    // The live Player owns a GameMode pointer. Polling it on the local tick
    // notices /gamemode before the next action and updates the cached radius.
    constexpr std::size_t attackSlot = gameModeAttackVTableIndices[1];
    alignas(void*) std::array<void*, attackSlot + 1> creativeVTable{};
    alignas(void*) std::array<void*, attackSlot + 1> survivalVTable{};
    survivalAttackFn = reinterpret_cast<std::uintptr_t>(&fakeBegin);
    creativeAttackFn = reinterpret_cast<std::uintptr_t>(&fakeColor);
    creativeInternalFn = reinterpret_cast<std::uintptr_t>(&fakeVertex);
    creativeVTable[attackSlot] = reinterpret_cast<void*>(creativeAttackFn);
    survivalVTable[attackSlot] = reinterpret_cast<void*>(survivalAttackFn);
    void* creativeGameMode[1]{creativeVTable.data()};
    void* survivalGameMode[1]{survivalVTable.data()};
    assert(looksLikeGameMode(creativeGameMode) && looksLikeGameMode(survivalGameMode));
    gameModeFieldOffset.store(off::Player::mGameMode);
    gameModeTickCounter.store(0);
    lastGameModeScanTick.store(0);
    lastPolledPlayer.store(nullptr);
    fresh();
    player.setGameMode(creativeGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    tick(player.ptr());
    assert(modeFromGameMode(creativeGameMode) == ModeHint::Creative);
    assert(mod.effectiveReach() == creativeReach && state.reach == creativeReach);
    player.setGameMode(survivalGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(true); // deliberately stale: polling must win
    tick(player.ptr());
    assert(modeFromGameMode(survivalGameMode) == ModeHint::Survival);
    assert(mod.effectiveReach() == survivalReach && state.reach == survivalReach);

    // With signatures unavailable, vtable identities learned from validated
    // live reads still work; an attack label cannot poison them.
    survivalAttackFn = creativeAttackFn = creativeInternalFn = 0;
    bedrocktools::events::AttackEvent wrongLabel{
        bedrocktools::events::AttackKind::GameMode, survivalGameMode, nullptr, nullptr, nullptr};
    bedrocktools::events::bus().publish(wrongLabel);
    assert(modeFromGameMode(survivalGameMode) == ModeHint::Survival);
    player.setGameMode(creativeGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    fresh();
    tick(player.ptr());
    assert(mod.effectiveReach() == creativeReach);
    player.setGameMode(survivalGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(true);
    tick(player.ptr());
    assert(mod.effectiveReach() == survivalReach);

    // A minor patch that moves the Player field is handled by the bounded scan.
    survivalAttackFn = reinterpret_cast<std::uintptr_t>(&fakeBegin);
    creativeAttackFn = reinterpret_cast<std::uintptr_t>(&fakeColor);
    creativeInternalFn = reinterpret_cast<std::uintptr_t>(&fakeVertex);
    constexpr std::size_t unknownOffset = 0xA40;
    player.setGameMode(nullptr);
    put(player.data, unknownOffset, reinterpret_cast<std::uintptr_t>(creativeGameMode));
    gameModeFieldOffset.store(off::Player::mGameMode);
    gameModeTickCounter.store(0);
    lastGameModeScanTick.store(0);
    lastPolledPlayer.store(nullptr);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    fresh();
    tick(player.ptr());
    assert(gameModeFieldOffset.load() == unknownOffset);
    assert(mod.effectiveReach() == creativeReach && state.reach == creativeReach);
    put(player.data, unknownOffset, std::uintptr_t{0});
    player.setGameMode(survivalGameMode);
    gameModeFieldOffset.store(off::Player::mGameMode);
    gameModeTickCounter.store(0);
    lastGameModeScanTick.store(0);
    lastPolledPlayer.store(nullptr);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    tick(player.ptr());
    assert(mod.effectiveReach() == survivalReach && state.reach == survivalReach);

    // Config round trip: legacy `reach` and `autoReach` keys are ignored on
    // load and removed on save. No manual Reach control can be restored.
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    nlohmann::json settings;
    settings["reach"] = 4.5f;
    settings["lineWidth"] = 0.2f;
    settings["effectStyle"] = "2";
    settings["inRangeColor"] = "#80123456";
    settings["outOfRangeColor"] = "#ABCDEF";
    settings["autoReach"] = false;
    settings["showSelf"] = false;
    settings["showTargets"] = false;
    settings["showPlayers"] = false;
    settings["showMobs"] = true;
    settings["seeThrough"] = true;
    mod.loadConfig(settings);
    assert(!mod.showInLegacyMenu("reach") && !mod.showInLegacyMenu("autoReach"));
    assert(mod.showInLegacyMenu("lineWidth"));
    assert(mod.effectiveReach() == survivalReach && mod.lineWidth == 0.2f && mod.style == 2);
    assert(mod.inRangeColor == 0x80123456u && mod.outOfRangeColor == 0xFFABCDEFu);
    assert(!mod.showSelf && !mod.showTargets && !mod.showPlayers && mod.showMobs && mod.seeThrough);
    nlohmann::json invalid;
    invalid["inRangeColor"] = "#not-a-color";
    mod.loadConfig(invalid);
    assert(mod.inRangeColor == 0x80123456u);
    nlohmann::json saved;
    mod.saveConfig(saved);
    assert(!saved.contains("reach") && !saved.contains("autoReach"));
    assert(saved["lineWidth"].get<float>() == 0.2f);
    assert(saved["inRangeColor"].get<std::string>() == "#80123456");
    assert(saved["outOfRangeColor"].get<std::string>() == "#FFABCDEF");
    assert(saved["effectStyle"].get<std::string>() == jumpcircle::styleRadioValue(2));
    assert(saved["seeThrough"].get<bool>());
    mod.loadConfig(saved);
    assert(mod.effectiveReach() == survivalReach && mod.style == 2 && mod.inRangeColor == 0x80123456u);
    for (int i = 0; i < jumpcircle::styleCount; ++i) {
        nlohmann::json choice;
        choice["effectStyle"] = std::to_string(i);
        mod.loadConfig(choice);
        assert(mod.style == i);
        nlohmann::json roundTrip;
        mod.saveConfig(roundTrip);
        assert(jumpcircle::styleIndex(roundTrip["effectStyle"].get<std::string>()) == i);
    }
    assert(safeReach(survivalReach) == survivalReach);
    assert(safeReach(creativeReach) == creativeReach);
    assert(safeWidth(saved["lineWidth"].get<float>()) == 0.2f);

    // A stale snapshot (pause / world change) draws nothing; disable clears it.
    mod.showSelf = true;
    mod.showTargets = true;
    resetDraw();
    {
        std::lock_guard lock(stateMutex);
        state.hasPlayer = true;
        state.lastTick = Clock::now() - std::chrono::seconds(1);
    }
    render(renderer.data(), screen.data());
    assert(begins == 0 && flushes == 0);
    mod.onDisable();
    assert(!state.hasPlayer && state.targets.empty() && trackedPlayer == nullptr);
    return 0;
}
