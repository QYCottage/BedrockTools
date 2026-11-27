#include "hitrange.hpp"
#include "hitrange_geometry.hpp"
#include "hitbox_interp.hpp"
#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace bedrocktools::sdk;
namespace off = bedrocktools::sdk::offsets;
using Clock = std::chrono::steady_clock;
using Begin = void (*)(void*, void*, int, int, int);
using TessColor = void (*)(void*, float, float, float, float);
using Vertex = void (*)(void*, float, float, float);
using RenderMesh = void (*)(void*, void*, void*, char*);
using RenderLevel = void (*)(void*, void*, void*);

// BlockSource::isSolidBlockingBlock(BlockPos const&), the same native query
// used by the Hitbox wall-occlusion pass.
struct BlockPosI {
    int x, y, z;
};
using IsSolidBlockingBlock = bool (*)(void*, const BlockPosI&);

struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};
struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};
using FetchNearby = ActorVec (*)(void*, void*, int);

struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;
    explicit HashedString(const char* name) : value(name) {
        hash = 0xCBF29CE484222325ULL;
        for (char c : value) hash = static_cast<unsigned char>(c) ^ (0x100000001B3ULL * hash);
    }
};
// Non-owning mirror of the game-owned MaterialPtr; its non-trivial destructor
// is required for the game's return ABI.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& o) noexcept : data{o.data[0], o.data[1]} { o.data[0] = o.data[1] = nullptr; }
    MaterialPtr& operator=(MaterialPtr&& o) noexcept {
        data[0] = o.data[0]; data[1] = o.data[1]; o.data[0] = o.data[1] = nullptr; return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return data[0] != nullptr; }
};

constexpr auto pauseTimeout = std::chrono::milliseconds(300);
// A hit can briefly remove a target from the game's nearby-actor result. The
// observed hit frames lose one ring for about 100 ms; hold it just long enough
// to span that gap without leaving a stale ring behind.
constexpr auto targetHoldWindow = std::chrono::milliseconds(180);
constexpr float scanPadding = 16.0f;
constexpr std::size_t maxScannedTargets = 64;
constexpr std::size_t maxTargetRings = 8;
// GameMode::attack lives in slot 15 on the 1.26.60 Windows layout and slot 16
// on the Android layout used by these hooks. Check both; a pointer is accepted
// only after its vtable passes the signature/vtable checks below.
constexpr std::size_t gameModeAttackVTableIndices[] = {15, 16};
// GameMode is a pointer-sized field near the end of Player. A bounded scan
// finds it if a minor game patch shifts the layout; it only runs until a
// vtable-validated pointer is found (or once every 20 ticks while unknown).
constexpr std::size_t gameModeScanStart = 0x900;
constexpr std::size_t gameModeScanEnd = 0xB40;
constexpr std::size_t gameModeScanChunk = 0x40;
constexpr std::uint64_t gameModeScanInterval = 20;

enum class ModeHint { Unknown, Survival, Creative };

HitRangeModule* module = nullptr;
Begin tessBegin = nullptr;
TessColor tessColor = nullptr;
Vertex tessVertex = nullptr;
RenderMesh renderMesh = nullptr;
RenderLevel originalRender = nullptr;
FetchNearby fetchNearby = nullptr;
IsSolidBlockingBlock isSolidBlockingBlock = nullptr;
std::uintptr_t survivalAttackFn = 0;
std::uintptr_t creativeAttackFn = 0;
std::uintptr_t creativeInternalFn = 0;
std::atomic<void**> learnedSurvivalVTable{nullptr};
std::atomic<void**> learnedCreativeVTable{nullptr};
std::atomic<std::size_t> gameModeFieldOffset{off::Player::mGameMode};
std::atomic<std::uint64_t> gameModeTickCounter{0};
std::atomic<std::uint64_t> lastGameModeScanTick{0};
std::atomic<void*> lastPolledPlayer{nullptr};
std::uintptr_t materialGroup = 0;
MaterialPtr depthMaterial;
MaterialPtr throughMaterial;

struct TargetState {
    void* actor = nullptr;
    hitbox::Track track{};
    Clock::time_point sampleTime{};
    Clock::time_point lastSeen{};
    AABB box{};
    Vec3 stableCenter{};
    bool inRange = false;
    bool held = false;
    hitrange::VerdictHold hold{};
};
struct State {
    bool hasPlayer = false;
    bool targetPresent = false;
    bool inRange = false;
    const void* hittableActor = nullptr;
    float reach = hitrange::survivalReach;
    hitbox::Track selfTrack{};
    Vec3 selfStableCenter{};
    Clock::time_point selfSampleTime{};
    Clock::time_point lastTick{};
    hitrange::VerdictHold verdictHold{};
    std::vector<TargetState> targets;
};
std::mutex stateMutex;
State state;
void* trackedPlayer = nullptr;

struct RecentAttack {
    void* target = nullptr;
    Clock::time_point at{};
};
std::mutex recentAttackMutex;
RecentAttack recentAttack;

std::uintptr_t resolveAdrp(std::uint32_t* p, std::size_t count, std::uint32_t reg) {
    for (std::size_t i = 0; i < count; ++i) {
        auto insn = p[i];
        if ((insn & 0x1f) != reg) continue;
        if ((insn & 0x9f000000) == 0x90000000) {
            auto bits = ((static_cast<std::uint64_t>((insn >> 3) & 0x1ffffc) |
                          static_cast<std::uint64_t>((insn >> 29) & 3)) << 43);
            auto page = (reinterpret_cast<std::uintptr_t>(&p[i]) & ~0xfffULL) +
                        (static_cast<std::int64_t>(bits) >> 31);
            for (std::size_t j = i + 1; j < count; ++j) {
                auto add = p[j];
                if ((add & 0xff000000) == 0x91000000 && ((add >> 5) & 31) == reg && (add & 31) == reg) {
                    auto imm = (add >> 10) & 0xfff;
                    if (add & 0x400000) imm <<= 12;
                    return page + imm;
                }
                if ((add & 31) == reg) break;
            }
        }
        if ((insn & 0x9f000000) == 0x10000000) {
            auto bits = (static_cast<std::uint64_t>(((insn >> 3) & 0x1ffffc) | (insn >> 29)) << 43);
            return reinterpret_cast<std::uintptr_t>(&p[i]) + (static_cast<std::int64_t>(bits) >> 43);
        }
    }
    return 0;
}

// Reading a candidate GameMode pointer must not crash if a minor game patch
// shifts the Player layout. process_vm_readv on our own pid safely returns a
// short read for an unmapped address; using the raw syscall also avoids a
// link-time dependency on an NDK API-level gate.
template <typename T>
bool safeRead(const void* source, T& destination) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    if (address < 0x10000 || address % alignof(T) != 0) return false;
    iovec local{&destination, sizeof(T)};
    iovec remote{const_cast<void*>(source), sizeof(T)};
    const long read = ::syscall(__NR_process_vm_readv, ::getpid(),
                                &local, 1, &remote, 1, 0L);
    return read == static_cast<long>(sizeof(T));
}

void** gameModeVTable(void* gameMode) noexcept {
    if (reinterpret_cast<std::uintptr_t>(gameMode) < 0x10000) return nullptr;
    void** vtable = nullptr;
    if (!safeRead(gameMode, vtable) ||
        reinterpret_cast<std::uintptr_t>(vtable) < 0x10000) return nullptr;
    return vtable;
}

bool looksLikeGameMode(void* gameMode) noexcept {
    void** vtable = gameModeVTable(gameMode);
    if (!vtable || reinterpret_cast<std::uintptr_t>(vtable) % alignof(void*) != 0) return false;
    // At least one of the two known attack slots must contain a plausible
    // code pointer. This rejects random integers at stale layout offsets.
    for (const auto slot : gameModeAttackVTableIndices) {
        void* attack = nullptr;
        if (!safeRead(vtable + slot, attack)) continue;
        if (reinterpret_cast<std::uintptr_t>(attack) >= 0x10000) return true;
    }
    return false;
}

// The game-mode object's attack vtable slot differs between SurvivalMode and
// the base/creative GameMode. Signature matching works immediately after a
// mode switch; learned vtable identities keep detection working on builds
// where one of the attack signatures is unavailable.
ModeHint modeFromGameMode(void* gameMode, bool allowLearned = true) {
    void** vtable = gameModeVTable(gameMode);
    if (!vtable) return ModeHint::Unknown;
    for (const auto slot : gameModeAttackVTableIndices) {
        void* attack = nullptr;
        if (!safeRead(vtable + slot, attack)) continue;
        const auto address = reinterpret_cast<std::uintptr_t>(attack);
        if (survivalAttackFn && address == survivalAttackFn) return ModeHint::Survival;
        if ((creativeAttackFn && address == creativeAttackFn) ||
            (creativeInternalFn && address == creativeInternalFn)) return ModeHint::Creative;
    }
    if (allowLearned) {
        if (vtable == learnedSurvivalVTable.load(std::memory_order_relaxed)) return ModeHint::Survival;
        if (vtable == learnedCreativeVTable.load(std::memory_order_relaxed)) return ModeHint::Creative;
    }
    return ModeHint::Unknown;
}

void learnGameModeVTable(void* gameMode, bool creative) {
    void** vtable = gameModeVTable(gameMode);
    if (!vtable) return;
    (creative ? learnedCreativeVTable : learnedSurvivalVTable)
        .store(vtable, std::memory_order_relaxed);
}

bool acceptGameMode(void* candidate) {
    if (!looksLikeGameMode(candidate)) return false;
    const auto hint = modeFromGameMode(candidate);
    if (hint == ModeHint::Unknown) return false;
    if (module) {
        module->setGameMode(candidate);
        // A validated live Player read is authoritative: change the radius now,
        // without waiting for debounced event hints.
        module->setCreativeMode(hint == ModeHint::Creative);
    }
    // Only a signature-confirmed identity may teach future reads. Event labels
    // from an attack are not allowed to poison the learned vtable cache.
    const auto direct = modeFromGameMode(candidate, false);
    if (direct != ModeHint::Unknown)
        learnGameModeVTable(candidate, direct == ModeHint::Creative);
    return true;
}

// Minor Bedrock patches can shift Player::mGameMode. A bounded, read-only scan
// finds the field by requiring both a plausible vtable and a known mode
// signature/vtable. It is throttled so normal ticks use one cached offset.
std::size_t findGameModeField(void* rawPlayer, void*& found) {
    const auto* bytes = reinterpret_cast<const std::byte*>(rawPlayer);
    for (std::size_t base = gameModeScanStart; base < gameModeScanEnd;
         base += gameModeScanChunk) {
        alignas(void*) std::array<std::byte, gameModeScanChunk> chunk{};
        if (!safeRead(bytes + base, chunk)) continue;
        for (std::size_t delta = 0; delta + sizeof(void*) <= chunk.size();
             delta += sizeof(void*)) {
            void* candidate = nullptr;
            std::memcpy(&candidate, chunk.data() + delta, sizeof(candidate));
            if (acceptGameMode(candidate)) {
                found = candidate;
                return base + delta;
            }
        }
    }
    return 0;
}

// Poll the live Player object every local tick rather than waiting for the
// next attack/build/interact event: changing /gamemode immediately replaces
// the GameMode pointer (or its vtable), so the radius changes on that tick.
void refreshGameModeFromPlayer(void* rawPlayer) {
    if (!module || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x10000) return;
    if (lastPolledPlayer.exchange(rawPlayer, std::memory_order_relaxed) != rawPlayer) {
        gameModeFieldOffset.store(off::Player::mGameMode, std::memory_order_relaxed);
        lastGameModeScanTick.store(0, std::memory_order_relaxed);
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(rawPlayer);
    static constexpr std::size_t kCandidates[] = {
        off::Player::mGameMode, off::Player::mGameModeLegacy, off::Player::mGameModeAlt,
    };
    const auto cachedOffset = gameModeFieldOffset.load(std::memory_order_relaxed);
    void* candidate = nullptr;
    if (cachedOffset && safeRead(bytes + cachedOffset, candidate) &&
        acceptGameMode(candidate)) return;
    for (const auto offset : kCandidates) {
        if (safeRead(bytes + offset, candidate) && acceptGameMode(candidate)) {
            gameModeFieldOffset.store(offset, std::memory_order_relaxed);
            return;
        }
    }
    const auto tick = gameModeTickCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto lastScan = lastGameModeScanTick.load(std::memory_order_relaxed);
    if (tick > 1 && lastScan <= tick && tick - lastScan < gameModeScanInterval) return;
    lastGameModeScanTick.store(tick, std::memory_order_relaxed);
    void* found = nullptr;
    if (const auto offset = findGameModeField(rawPlayer, found))
        gameModeFieldOffset.store(offset, std::memory_order_relaxed);
}

MaterialPtr getMaterial(const char* name) {
    if (!materialGroup) return {};
    auto** vtable = *reinterpret_cast<void***>(materialGroup);
    if (!vtable || !vtable[off::VTable::RenderMaterialGroup_getMaterial]) return {};
    HashedString hashed(name);
    using GetMaterial = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterial>(vtable[off::VTable::RenderMaterialGroup_getMaterial])(
        reinterpret_cast<void*>(materialGroup), &hashed);
}

void ensureMaterials() {
    // name_tag_depth_tested has alpha blending and still respects terrain
    // depth, the same material the other circle modules use.
    if (!depthMaterial) depthMaterial = getMaterial("name_tag_depth_tested");
    if (!module->seeThrough || throughMaterial) return;
    static constexpr const char* kThroughWalls[] = {
        "ui_fill_color",
        "ui_textured_and_glcolor",
    };
    for (const char* candidate : kThroughWalls) {
        throughMaterial = getMaterial(candidate);
        if (throughMaterial) return;
    }
    // A build without either UI material degrades to the normal depth-tested
    // ring instead of drawing nothing at all.
    throughMaterial.data[0] = depthMaterial.data[0];
    throughMaterial.data[1] = depthMaterial.data[1];
}

void clearState() {
    {
        std::lock_guard attackLock(recentAttackMutex);
        recentAttack = {};
    }
    std::lock_guard lock(stateMutex);
    state = {};
    trackedPlayer = nullptr;
}

// Resolve the dimension's BlockSource through safely checked pointers. The
// module only reads it; it never changes world state or sends packets.
void* occlusionRegion(void* rawPlayer) noexcept {
    if (!rawPlayer || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) return nullptr;
    const auto* bytes = reinterpret_cast<const std::byte*>(rawPlayer);
    void* dimension = nullptr;
    if (!safeRead(bytes + off::Actor::mDimension, dimension) ||
        reinterpret_cast<std::uintptr_t>(dimension) < 0x1000) return nullptr;
    void* region = nullptr;
    if (!safeRead(reinterpret_cast<const std::byte*>(dimension) + off::Dimension::mBlockSource,
                  region) ||
        reinterpret_cast<std::uintptr_t>(region) < 0x1000) return nullptr;
    return region;
}

bool nativeSolidBlock(const void* context, int x, int y, int z) noexcept {
    if (!isSolidBlockingBlock || !context) return false;
    const BlockPosI position{x, y, z};
    return isSolidBlockingBlock(const_cast<void*>(context), position);
}

void tick(void* rawPlayer) {
    if (!module || !module->enabled || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) {
        clearState();
        return;
    }
    refreshGameModeFromPlayer(rawPlayer);
    auto* player = reinterpret_cast<bedrocktools::sdk::Actor*>(rawPlayer);
    const AABB playerBox = player->bounds();
    if (!hitrange::validBox(playerBox)) { clearState(); return; }

    const auto now = Clock::now();
    const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    // Cache the radius for this tick as well: the render hook must not read a
    // mode hint that changes between the tick and the draw call.
    const float reach = module->effectiveReach();
    // Problems 5-7: only the 3D eye position and native sight test decide
    // green. Camera rotation/crosshair HitResult are deliberately not read, and
    // the exposed-point sampler covers partial visibility and targets above or
    // below the player without accepting a wall-hidden target.
    const Vec3 eye = hitrange::eyePosition(playerBox);
    void* region = occlusionRegion(rawPlayer);
    const hitrange::SightTest sight{
        region, (region && isSolidBlockingBlock) ? &nativeSolidBlock : nullptr};

    RecentAttack attack{};
    {
        std::lock_guard attackLock(recentAttackMutex);
        attack = recentAttack;
    }
    const bool attackActive = attack.target && attack.at != Clock::time_point{} &&
        reinterpret_cast<std::uintptr_t>(attack.target) >= 0x1000 &&
        now - attack.at <= targetHoldWindow;

    // Copy the previous interpolation state, then sample the game outside the
    // lock so the render hook never waits on an actor fetch.
    bool hadPlayer = false;
    hitbox::Track selfTrack{};
    Vec3 selfStableCenter{};
    Clock::time_point selfSampleTime{};
    Clock::time_point lastTick{};
    hitrange::VerdictHold verdictHold{};
    std::vector<TargetState> previousTargets;
    {
        std::lock_guard lock(stateMutex);
        if (trackedPlayer != rawPlayer) {
            state = {};
            trackedPlayer = rawPlayer;
        }
        hadPlayer = state.hasPlayer;
        selfTrack = state.selfTrack;
        selfStableCenter = state.selfStableCenter;
        selfSampleTime = state.selfSampleTime;
        lastTick = state.lastTick;
        verdictHold = state.verdictHold;
        previousTargets = state.targets;
    }
    const bool paused = lastTick != Clock::time_point{} && now - lastTick > pauseTimeout;
    if (paused) {
        selfTrack.hasPrev = false;
        selfTrack.prevCenter = selfTrack.curCenter;
        for (auto& target : previousTargets) {
            target.track.hasPrev = false;
            target.track.prevCenter = target.track.curCenter;
        }
    }
    const float selfElapsed = selfSampleTime == Clock::time_point{}
        ? 0.05f : std::chrono::duration<float>(now - selfSampleTime).count();
    const Vec3 selfSample = hitrange::feetCenter(playerBox);
    if (!hadPlayer) {
        selfStableCenter = selfSample;
    } else {
        selfStableCenter = hitrange::stepToward(selfStableCenter, selfSample,
                                                hitrange::maxCenterStep);
    }
    if (hitbox::pushSample(selfTrack, selfStableCenter, selfElapsed))
        selfSampleTime = now;

    std::vector<TargetState> nextTargets;
    std::vector<hitrange::TargetCandidate> candidates;
    if (fetchNearby) {
        Vec3 extent{reach + scanPadding, reach + scanPadding, reach + scanPadding};
        const ActorVec actors = fetchNearby(rawPlayer, &extent, 1);
        if (actors.begin && actors.end > actors.begin) {
            const std::size_t count = std::min(
                static_cast<std::size_t>(actors.end - actors.begin), maxScannedTargets);
            for (std::size_t i = 0; i < count; ++i) {
                void* actor = actors.begin[i].mActor;
                if (!actor || actor == rawPlayer || reinterpret_cast<std::uintptr_t>(actor) < 0x1000) continue;
                const auto categories = reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->categories();
                const bool isPlayer = (categories & off::ActorCategories::IsPlayer) != 0;
                const bool isMob = !isPlayer && (categories & off::ActorCategories::IsMob) != 0;
                if (!((isPlayer && module->showPlayers) || (isMob && module->showMobs))) continue;
                const AABB box = reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->bounds();
                if (!hitrange::validBox(box)) continue;
                const hitrange::TargetCandidate candidate{actor, box};
                candidates.push_back(candidate);
                const bool rawInRange = hitrange::candidateHittable(
                    candidate, reach, eye, sight);
                if (nextTargets.size() >= maxTargetRings) continue; // keep scanning for the verdict

                TargetState target;
                target.actor = actor;
                target.box = box;
                target.lastSeen = now;
                target.held = false;
                const Vec3 desiredCenter = hitrange::feetCenter(box);
                auto old = std::find_if(previousTargets.begin(), previousTargets.end(),
                    [actor](const TargetState& t) { return t.actor == actor; });
                if (old != previousTargets.end()) {
                    target.track = old->track;
                    target.sampleTime = old->sampleTime;
                    target.hold = old->hold;
                    target.stableCenter = hitrange::stepToward(
                        old->stableCenter, desiredCenter, hitrange::maxCenterStep);
                    const float elapsed = target.sampleTime == Clock::time_point{}
                        ? 0.05f : std::chrono::duration<float>(now - target.sampleTime).count();
                    if (hitbox::pushSample(target.track, target.stableCenter, elapsed))
                        target.sampleTime = now;
                } else {
                    target.stableCenter = desiredCenter;
                    hitbox::pushSample(target.track, target.stableCenter, 0.05f);
                    target.sampleTime = now;
                }
                target.inRange = target.hold.update(rawInRange, nowSeconds);
                nextTargets.push_back(std::move(target));
            }
        }
    }

    // An attack can briefly make the nearby-actor fetch omit the target. Keep
    // the last stable ring and color rather than dropping/blinking it on every
    // hit. Held entries are render-only: the stale actor pointer is never read.
    for (const auto& old : previousTargets) {
        if (std::any_of(nextTargets.begin(), nextTargets.end(),
                        [&old](const TargetState& t) { return t.actor == old.actor; })) continue;
        const bool withinHold = old.lastSeen != Clock::time_point{} &&
                                now - old.lastSeen <= targetHoldWindow;
        const bool heldByAttack = attackActive && attack.target == old.actor;
        if (!withinHold && !heldByAttack) continue;
        if (nextTargets.size() >= maxTargetRings) break;
        TargetState held = old;
        held.held = true;
        nextTargets.push_back(std::move(held));
    }

    const auto verdict = hitrange::classify(candidates, reach, eye, sight);
    const bool stableInRange = verdictHold.update(verdict.inRange, nowSeconds);

    {
        std::lock_guard lock(stateMutex);
        if (trackedPlayer != rawPlayer) return; // a newer tick owns a different player
        state.hasPlayer = true;
        state.targetPresent = verdict.targetPresent || !nextTargets.empty();
        state.inRange = stableInRange;
        state.hittableActor = verdict.hittableActor;
        state.reach = reach;
        state.selfTrack = selfTrack;
        state.selfStableCenter = selfStableCenter;
        state.selfSampleTime = selfSampleTime;
        state.lastTick = now;
        state.verdictHold = verdictHold;
        state.targets = std::move(nextTargets);
    }
}

void render(void* renderer, void* screen) {
    if (!module || !module->enabled || !tessBegin || !tessColor || !tessVertex || !renderMesh ||
        reinterpret_cast<std::uintptr_t>(renderer) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(screen) < 0x1000) return;
    const auto now = Clock::now();
    State snapshot;
    {
        std::lock_guard lock(stateMutex);
        snapshot = state;
    }
    if (!snapshot.hasPlayer || snapshot.lastTick == Clock::time_point{} ||
        now - snapshot.lastTick > pauseTimeout) return;

    ensureMaterials();
    MaterialPtr* material = module->seeThrough ? &throughMaterial : &depthMaterial;
    if (!material || !(*material)) return;
    auto scr = reinterpret_cast<std::uintptr_t>(screen);
    auto tess = *reinterpret_cast<std::uintptr_t*>(scr + off::ScreenContext::mTessellator);
    auto lrp = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(renderer) + off::LevelRenderer::mLevelRendererPlayer);
    if (tess < 0x1000 || lrp < 0x1000) return;
    const auto cam = *reinterpret_cast<const Vec3*>(lrp + off::LevelRendererPlayer::mCamPos);
    bedrocktools::core::ScopedShaderColor shaderColor(screen);
    if (!shaderColor) return;

    struct DrawRing {
        Vec3 center;
        std::uint32_t color;
    };
    std::vector<DrawRing> rings;
    if (module->showSelf) {
        const float elapsed = snapshot.selfSampleTime == Clock::time_point{}
            ? 0.05f : std::chrono::duration<float>(now - snapshot.selfSampleTime).count();
        rings.push_back({hitrange::interpolatedCenter(snapshot.selfTrack, elapsed),
                         snapshot.inRange ? module->inRangeColor : module->outOfRangeColor});
    }
    if (module->showTargets) {
        for (const auto& target : snapshot.targets) {
            const float elapsed = target.sampleTime == Clock::time_point{}
                ? 0.05f : std::chrono::duration<float>(now - target.sampleTime).count();
            rings.push_back({hitrange::interpolatedCenter(target.track, elapsed),
                             target.inRange ? module->inRangeColor : module->outOfRangeColor});
        }
    }
    if (rings.empty()) return;

    // Use the radius sampled on the last client tick. A mode/event hint can no
    // longer resize the rings halfway through an attack frame.
    const float reach = hitrange::safeReach(snapshot.reach);
    const float width = hitrange::safeWidth(module->lineWidth);
    const auto style = static_cast<jumpcircle::Style>(
        std::clamp(module->style, 0, jumpcircle::styleCount - 1));
    tessBegin(reinterpret_cast<void*>(tess), nullptr, 1,
              static_cast<int>(rings.size()) * hitrange::vertexCount, 0);
    for (const auto& ring : rings) {
        const std::uint32_t color = ring.color;
        const float red = ((color >> 16) & 255) / 255.0f;
        const float green = ((color >> 8) & 255) / 255.0f;
        const float blue = (color & 255) / 255.0f;
        const float alpha = ((color >> 24) & 255) / 255.0f;
        hitrange::emitRing(ring.center, reach, width, style,
            [&](const Vec3& point, float strength) {
                tessColor(reinterpret_cast<void*>(tess), red, green, blue, strength * alpha);
                tessVertex(reinterpret_cast<void*>(tess), point.x - cam.x,
                           point.y - cam.y, point.z - cam.z);
            });
    }
    char pad[0x100]{};
    renderMesh(screen, reinterpret_cast<void*>(tess), material, pad);
}

void renderHook(void* renderer, void* screen, void* params) {
    if (originalRender) originalRender(renderer, screen, params);
    render(renderer, screen);
}
} // namespace

HitRangeModule::HitRangeModule()
    : Module("Hit Range", "Auto-sized ground circle (survival/creative) that turns green when any exposed part of a target is in 3D reach with clear sight, regardless of camera aim or vertical offset.") {
    module = this;
    hideInHudEditor = true;
}
HitRangeModule::~HitRangeModule() { if (module == this) module = nullptr; }

float HitRangeModule::effectiveReach() const {
    const auto hint = modeFromGameMode(m_gameMode.load(std::memory_order_relaxed), false);
    if (hint == ModeHint::Survival) return hitrange::survivalReach;
    if (hint == ModeHint::Creative) return hitrange::creativeReach;
    // Learned vtable identities and debounced non-attack events are fallbacks
    // for builds where the direct attack signatures are unavailable.
    return m_creative.load(std::memory_order_relaxed)
        ? hitrange::creativeReach : hitrange::survivalReach;
}
void HitRangeModule::setGameMode(void* gameMode) {
    m_gameMode.store(gameMode, std::memory_order_relaxed);
}
void HitRangeModule::setCreativeMode(bool creative) {
    observeGameMode(creative, true);
}
void HitRangeModule::observeGameMode(bool creative, bool authoritative) {
    bool stable;
    {
        std::lock_guard lock(m_modeMutex);
        if (authoritative) m_mode.reset(creative);
        else m_mode.observe(creative);
        stable = m_mode.creative;
    }
    m_creative.store(stable, std::memory_order_relaxed);
}

void HitRangeModule::onInit() {
    using bedrocktools::memory::resolve;
    using bedrocktools::memory::SignatureId;
    m_patchTarget = reinterpret_cast<void*>(resolve(SignatureId::RenderLevel));
    tessBegin = reinterpret_cast<Begin>(resolve(SignatureId::TessellatorBegin));
    tessColor = reinterpret_cast<TessColor>(resolve(SignatureId::TessellatorColor));
    tessVertex = reinterpret_cast<Vertex>(resolve(SignatureId::TessellatorVertex));
    auto mesh = resolve(SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!mesh) mesh = resolve(SignatureId::MeshHelpersRenderMeshImmediately);
    renderMesh = reinterpret_cast<RenderMesh>(mesh);
    fetchNearby = reinterpret_cast<FetchNearby>(resolve(SignatureId::ActorFetchNearbyActorsSorted));
    isSolidBlockingBlock = reinterpret_cast<IsSolidBlockingBlock>(
        resolve(SignatureId::BlockSourceIsSolidBlockingBlock));
    survivalAttackFn = resolve(SignatureId::SurvivalModeAttack);
    creativeAttackFn = resolve(SignatureId::GameModeAttack);
    creativeInternalFn = resolve(SignatureId::GameModeAttackInternal);
    auto group = resolve(SignatureId::RenderMaterialGroupCommon);
    if (group) {
        auto address = resolveAdrp(reinterpret_cast<std::uint32_t*>(group), 2, 0);
        if (address) materialGroup = address + off::MaterialGroup::mRenderMaterialGroupOffset;
    }
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tick(event.player); });
    // Signature-confirmed mode events are authoritative; other non-attack
    // events are only debounced fallback hints. Attack labels never resize the
    // rings: on Android they can be wrong for a single arm swing.
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [](auto& event) {
            if (!module) return;
            bool known = false;
            if (looksLikeGameMode(event.gameMode)) {
                const auto hint = modeFromGameMode(event.gameMode, false);
                if (hint != ModeHint::Unknown) {
                    const bool creative = hint == ModeHint::Creative;
                    module->setGameMode(event.gameMode);
                    module->setCreativeMode(creative);
                    learnGameModeVTable(event.gameMode, creative);
                    known = true;
                }
            }
            if (!known && event.action != bedrocktools::events::GameModeAction::Attack) {
                if (gameModeVTable(event.gameMode)) module->setGameMode(event.gameMode);
                module->observeGameMode(
                    event.modeKind == bedrocktools::events::GameModeType::GameMode, false);
            }
        });
    // A hit only latches the target's stable ring for the short hold window.
    // It does not override the range/LOS verdict, color, mode or radius.
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [](auto& event) {
            if (!module) return;
            if (event.target && reinterpret_cast<std::uintptr_t>(event.target) >= 0x1000) {
                std::lock_guard attackLock(recentAttackMutex);
                recentAttack = {event.target, Clock::now()};
            }
        });
}

void HitRangeModule::onEnable() {
    clearState();
    if (!m_patched && m_patchTarget)
        m_patched = bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(&renderHook),
                                                 reinterpret_cast<void**>(&originalRender)) != nullptr;
}
void HitRangeModule::onDisable() { clearState(); }

bool HitRangeModule::showInLegacyMenu(std::string_view key) const {
    // Guard against a stale launcher UI/config callback trying to put the
    // removed manual controls back into the generic module menu.
    if (key == "reach" || key == "autoReach") return false;
    return Module::showInLegacyMenu(key);
}

void HitRangeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("effectStyle") && j["effectStyle"].is_string())
        style = jumpcircle::styleIndex(j["effectStyle"].get<std::string>());
    if (j.contains("lineWidth")) {
        try { lineWidth = hitrange::safeWidth(j["lineWidth"].get<float>()); } catch (...) {}
    }
    if (j.contains("inRangeColor") && j["inRangeColor"].is_string())
        inRangeColor = hitrange::parseColor(j["inRangeColor"].get<std::string>(), inRangeColor);
    if (j.contains("outOfRangeColor") && j["outOfRangeColor"].is_string())
        outOfRangeColor = hitrange::parseColor(j["outOfRangeColor"].get<std::string>(), outOfRangeColor);
    if (j.contains("showSelf")) { try { showSelf = j["showSelf"].get<bool>(); } catch (...) {} }
    if (j.contains("showTargets")) { try { showTargets = j["showTargets"].get<bool>(); } catch (...) {} }
    if (j.contains("showPlayers")) { try { showPlayers = j["showPlayers"].get<bool>(); } catch (...) {} }
    if (j.contains("showMobs")) { try { showMobs = j["showMobs"].get<bool>(); } catch (...) {} }
    if (j.contains("seeThrough")) { try { seeThrough = j["seeThrough"].get<bool>(); } catch (...) {} }
}

void HitRangeModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    // Old configurations could contain a manual Reach slider or an Auto Reach
    // toggle. Neither exists now; erase the keys so custom configurations do
    // not recreate the removed control in the generic module menu.
    j.erase("reach");
    j.erase("autoReach");
    j["lineWidth"] = hitrange::safeWidth(lineWidth);
    j["effectStyle"] = jumpcircle::styleRadioValue(style);
    j["inRangeColor"] = hitrange::colorString(inRangeColor);
    j["outOfRangeColor"] = hitrange::colorString(outOfRangeColor);
    j["showSelf"] = showSelf;
    j["showTargets"] = showTargets;
    j["showPlayers"] = showPlayers;
    j["showMobs"] = showMobs;
    j["seeThrough"] = seeThrough;
}
