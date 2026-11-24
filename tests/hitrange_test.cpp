// Integration test for the Hit Range tick sampler, authoritative crosshair
// verdict, auto-detected survival/creative reach and ring renderer using fake
// game memory.
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

std::array<std::byte, 0x100> g_level{};
std::array<std::byte, 0x100> g_hitResult{};
void* g_pickedActor = nullptr;
std::vector<DistanceSortedActor> g_nearby;

void* fakeLevelGetHitResult(void*) { return g_hitResult.data(); }
void* fakeHitResultGetEntity(void*) { return g_pickedActor; }
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
void fakeVertex(void*, float x, float y, float z) {
    // Target rings are centered on entities away from the camera, so allow a
    // generous world-space bound rather than asserting a self-ring radius
    // here. Mode detection's 3/5 radius is checked via effectiveReach().
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
void setHit(int type, void* picked = nullptr) {
    put(g_hitResult, off::HitResult::mType, type);
    g_pickedActor = picked;
}
} // namespace

int main() {
    using namespace hitrange;
    const AABB playerBox{{-0.3f, 64.0f, -0.3f}, {0.3f, 65.8f, 0.3f}};
    const AABB nearBox{{2.0f, 64.0f, -0.3f}, {2.6f, 65.8f, 0.3f}};
    const AABB nearMobBox{{2.0f, 64.0f, 0.4f}, {2.6f, 65.8f, 1.0f}};
    const AABB farPlayerBox{{4.7f, 64.0f, -0.3f}, {5.3f, 65.8f, 0.3f}};
    const AABB farMobBox{{6.7f, 64.0f, -0.3f}, {7.3f, 65.8f, 0.3f}};
    const AABB frontBox{{-0.3f, 64.0f, 2.0f}, {0.3f, 65.8f, 2.6f}};
    const AABB behindBox{{-0.3f, 64.0f, -2.6f}, {0.3f, 65.8f, -2.0f}};

    FakeActor player, targetPlayer, targetMob;
    player.set(playerBox, off::ActorCategories::IsPlayer, g_level.data());
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);
    setHit(off::HitResult::TypeEntity, targetPlayer.ptr());
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
    mod.setCreativeMode(false);
    tessBegin = fakeBegin;
    tessColor = fakeColor;
    tessVertex = fakeVertex;
    renderMesh = fakeFlush;
    levelGetHitResult = fakeLevelGetHitResult;
    hitResultGetEntity = fakeHitResultGetEntity;
    fetchNearby = fakeFetch;
    materialGroup = 1;
    depthMaterial.data[0] = &vertices;
    throughMaterial.data[0] = &vertices;
    mod.showMobs = true;

    tick(player.ptr());
    assert(state.hasPlayer && state.game.available);
    assert(state.game.hit == GameHit::Entity);
    assert(state.hittableActor == targetPlayer.ptr());
    assert(state.targetPresent && state.inRange);
    assert(state.targets.size() == 2);
    assert(state.targets[0].inRange && !state.targets[1].inRange);
    assert(mod.effectiveReach() == survivalReach);

    render(renderer.data(), screen.data());
    assert(begins == 1 && flushes == 1);
    assert(lastCount == 3 * vertexCount && vertices == lastCount);
    assert(colors.size() == static_cast<std::size_t>(vertices));
    expectColor(0, mod.inRangeColor);                  // own ring: green
    expectColor(vertexCount, mod.inRangeColor);        // picked player: green
    expectColor(2 * vertexCount, mod.outOfRangeColor); // distant mob: red
    assert(materials.back() == &depthMaterial);
    assert((gpu.rgba == fake_render::Color{1, 1, 1, 1}));
    assert((color.rgba == fake_render::Color{.2f, .3f, .4f, .5f}) && color.dirty == 1);
    assert((legacy.rgba == fake_render::Color{.7f, .6f, .5f, .4f}));

    // Reported bug: an entity close to the player box but actually out of the
    // game's reach must NOT light the ring green.
    targetPlayer.set(farPlayerBox, off::ActorCategories::IsPlayer);
    setHit(off::HitResult::TypeEntityOutOfRange, targetPlayer.ptr());
    tick(player.ptr());
    assert(state.game.hit == GameHit::EntityOutOfRange);
    assert(state.targetPresent && !state.inRange);
    assert(!state.targets[0].inRange && !state.targets[1].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    assert(lastCount == 3 * vertexCount);
    expectColor(0, mod.outOfRangeColor);
    expectColor(vertexCount, mod.outOfRangeColor);
    expectColor(2 * vertexCount, mod.outOfRangeColor);

    // Even with the target back inside the old 3-block AABB test, an explicit
    // out-of-range / block / no-hit verdict stays red.
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    setHit(off::HitResult::TypeEntityOutOfRange, targetPlayer.ptr());
    tick(player.ptr());
    assert(!state.inRange && !state.targets[0].inRange);
    setHit(off::HitResult::TypeBlock, targetPlayer.ptr());
    tick(player.ptr());
    assert(!state.targetPresent && !state.inRange && !state.targets[0].inRange);
    setHit(off::HitResult::TypeNoHit);
    tick(player.ptr());
    assert(!state.targetPresent && !state.inRange && !state.targets[0].inRange);

    // Only the exact entity the game picked gets a green target ring.
    targetMob.set(nearMobBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}, {targetMob.ptr(), 2.6f, 0.0f}};
    setHit(off::HitResult::TypeEntity, targetPlayer.ptr());
    tick(player.ptr());
    assert(state.inRange && state.targets[0].inRange && !state.targets[1].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);
    expectColor(2 * vertexCount, mod.outOfRangeColor);

    // If the picked-entity getter is missing the self ring still follows the
    // TypeEntity verdict, but no individual target ring is falsely green.
    hitResultGetEntity = nullptr;
    tick(player.ptr());
    assert(state.inRange && state.hittableActor == nullptr);
    assert(!state.targets[0].inRange && !state.targets[1].inRange);
    hitResultGetEntity = fakeHitResultGetEntity;

    // Without the actor fetch, the game verdict alone still decides.
    fetchNearby = nullptr;
    setHit(off::HitResult::TypeEntity, targetPlayer.ptr());
    tick(player.ptr());
    assert(state.targets.empty() && state.targetPresent && state.inRange);
    setHit(off::HitResult::TypeEntityOutOfRange, targetPlayer.ptr());
    tick(player.ptr());
    assert(state.targetPresent && !state.inRange);
    setHit(off::HitResult::TypeNoHit);
    tick(player.ptr());
    assert(!state.targetPresent && !state.inRange);

    // Fallback when the hit-result signature is unavailable: an eye-ray test
    // replaces the old AABB-distance test, so behind/to-the-side mobs stay red.
    fetchNearby = fakeFetch;
    levelGetHitResult = nullptr;
    hitResultGetEntity = nullptr;
    targetPlayer.set(frontBox, off::ActorCategories::IsPlayer);
    targetMob.set(behindBox, off::ActorCategories::IsMob);
    g_nearby = {{targetPlayer.ptr(), 2.3f, 0.0f}, {targetMob.ptr(), 2.3f, 0.0f}};
    tick(player.ptr());
    assert(!state.game.available && state.game.hit == GameHit::Unknown);
    assert(state.targetPresent && state.inRange);
    assert(state.targets[0].inRange && !state.targets[1].inRange);
    resetDraw();
    render(renderer.data(), screen.data());
    expectColor(0, mod.inRangeColor);
    expectColor(vertexCount, mod.inRangeColor);
    expectColor(2 * vertexCount, mod.outOfRangeColor);
    levelGetHitResult = fakeLevelGetHitResult;
    hitResultGetEntity = fakeHitResultGetEntity;
    targetPlayer.set(nearBox, off::ActorCategories::IsPlayer);
    targetMob.set(farMobBox, off::ActorCategories::IsMob);
    setHit(off::HitResult::TypeEntity, targetPlayer.ptr());

    // Group toggles change which rings are drawn, not the authoritative color.
    mod.showMobs = false;
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.inRange);
    mod.showPlayers = false;
    mod.showMobs = true;
    tick(player.ptr());
    assert(state.targets.size() == 1 && state.inRange);
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

    // Reach is always detected, never configured: survival is 3, creative is
    // 5, and a mode event changes the radius immediately.
    assert(mod.effectiveReach() == survivalReach);
    bedrocktools::events::GameModeActionEvent creative{
        bedrocktools::events::GameModeType::GameMode,
        bedrocktools::events::GameModeAction::Attack, nullptr};
    bedrocktools::events::bus().publish(creative);
    assert(mod.effectiveReach() == creativeReach);
    bedrocktools::events::AttackEvent survival{
        bedrocktools::events::AttackKind::SurvivalMode, nullptr, nullptr, nullptr, nullptr};
    bedrocktools::events::bus().publish(survival);
    assert(mod.effectiveReach() == survivalReach);
    assert(modeFromGameMode(nullptr) == ModeHint::Unknown);

    // The live Player owns a GameMode pointer. Polling it on the local tick
    // notices a /gamemode switch before the next attack/build event.
    constexpr std::size_t attackSlot = gameModeAttackVTableIndices[1];
    alignas(void*) std::array<void*, attackSlot + 1> creativeVTable{};
    alignas(void*) std::array<void*, attackSlot + 1> survivalVTable{};
    learnedSurvivalVTable.store(nullptr);
    learnedCreativeVTable.store(nullptr);
    survivalAttackFn = reinterpret_cast<std::uintptr_t>(&fakeBegin);
    creativeAttackFn = reinterpret_cast<std::uintptr_t>(&fakeColor);
    creativeInternalFn = reinterpret_cast<std::uintptr_t>(&fakeVertex);
    creativeVTable[attackSlot] = reinterpret_cast<void*>(creativeAttackFn);
    survivalVTable[attackSlot] = reinterpret_cast<void*>(survivalAttackFn);
    void* creativeGameMode[1]{creativeVTable.data()};
    void* survivalGameMode[1]{survivalVTable.data()};
    assert(looksLikeGameMode(creativeGameMode) && looksLikeGameMode(survivalGameMode));
    player.setGameMode(creativeGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false); // deliberately stale: polling must win
    tick(player.ptr());
    assert(modeFromGameMode(creativeGameMode) == ModeHint::Creative);
    assert(mod.effectiveReach() == creativeReach);
    player.setGameMode(survivalGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(true);  // deliberately stale again
    tick(player.ptr());
    assert(modeFromGameMode(survivalGameMode) == ModeHint::Survival);
    assert(mod.effectiveReach() == survivalReach);

    // When attack signatures are unavailable, vtable identities learned from
    // mode events still detect both modes immediately.
    survivalAttackFn = creativeAttackFn = creativeInternalFn = 0;
    bedrocktools::events::GameModeActionEvent learnCreative{
        bedrocktools::events::GameModeType::GameMode,
        bedrocktools::events::GameModeAction::Attack, creativeGameMode};
    bedrocktools::events::bus().publish(learnCreative);
    bedrocktools::events::AttackEvent learnSurvival{
        bedrocktools::events::AttackKind::SurvivalMode,
        survivalGameMode, nullptr, nullptr, nullptr};
    bedrocktools::events::bus().publish(learnSurvival);
    player.setGameMode(creativeGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    tick(player.ptr());
    assert(mod.effectiveReach() == creativeReach);
    player.setGameMode(survivalGameMode);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(true);
    tick(player.ptr());
    assert(mod.effectiveReach() == survivalReach);

    // A minor patch that moves the Player field is handled by the bounded
    // scan: the mode is still found on the first tick, with no event needed.
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
    tick(player.ptr());
    assert(gameModeFieldOffset.load() == unknownOffset);
    assert(mod.effectiveReach() == creativeReach);
    put(player.data, unknownOffset, std::uintptr_t{0});
    player.setGameMode(survivalGameMode);
    gameModeFieldOffset.store(off::Player::mGameMode);
    gameModeTickCounter.store(0);
    lastGameModeScanTick.store(0);
    lastPolledPlayer.store(nullptr);
    mod.setGameMode(nullptr);
    mod.setCreativeMode(false);
    tick(player.ptr());
    assert(mod.effectiveReach() == survivalReach);

    // A touch attack that connects is authoritative for a short window even
    // if the stored hit result says the target is too far away.
    bedrocktools::events::AttackEvent confirmed{
        bedrocktools::events::AttackKind::SurvivalMode,
        survivalGameMode,
        reinterpret_cast<bedrocktools::sdk::Actor*>(targetPlayer.ptr()),
        nullptr, nullptr};
    bedrocktools::events::bus().publish(confirmed);
    setHit(off::HitResult::TypeEntityOutOfRange, targetPlayer.ptr());
    tick(player.ptr());
    assert(state.game.available && state.game.hit == GameHit::Entity);
    assert(state.inRange && state.targets[0].inRange);
    mod.setGameMode(nullptr);

    // Config round trip: legacy `reach` and `autoReach` keys are ignored on
    // load and removed on save. No manual Reach control can be restored.
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
