#include "hitrange.hpp"
#include "hitrange_geometry.hpp"
#include "hitbox_interp.hpp"
#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
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
using LevelGetHitResult = void* (*)(void*);

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
constexpr float scanPadding = 16.0f;
constexpr std::size_t maxScannedTargets = 64;
constexpr std::size_t maxTargetRings = 8;

HitRangeModule* module = nullptr;
Begin tessBegin = nullptr;
TessColor tessColor = nullptr;
Vertex tessVertex = nullptr;
RenderMesh renderMesh = nullptr;
RenderLevel originalRender = nullptr;
LevelGetHitResult levelGetHitResult = nullptr;
FetchNearby fetchNearby = nullptr;
std::uintptr_t materialGroup = 0;
MaterialPtr depthMaterial;
MaterialPtr throughMaterial;

struct TargetState {
    void* actor = nullptr;
    hitbox::Track track{};
    Clock::time_point sampleTime{};
    AABB box{};
    bool inRange = false;
};
struct State {
    bool hasPlayer = false;
    bool targetPresent = false;
    bool inRange = false;
    hitbox::Track selfTrack{};
    Clock::time_point selfSampleTime{};
    Clock::time_point lastTick{};
    std::vector<TargetState> targets;
};
std::mutex stateMutex;
State state;
void* trackedPlayer = nullptr;

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
    std::lock_guard lock(stateMutex);
    state = {};
    trackedPlayer = nullptr;
}

void readGameVerdict(void* rawPlayer, bool& gameInRange, bool& gameTargetPresent) {
    if (!levelGetHitResult || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) return;
    auto* level = reinterpret_cast<bedrocktools::sdk::Actor*>(rawPlayer)->level();
    if (reinterpret_cast<std::uintptr_t>(level) < 0x1000) return;
    void* hit = levelGetHitResult(reinterpret_cast<void*>(level));
    if (reinterpret_cast<std::uintptr_t>(hit) < 0x1000) return;
    const int type = *reinterpret_cast<const int*>(
        reinterpret_cast<std::uintptr_t>(hit) + off::HitResult::mType);
    if (type == off::HitResult::TypeEntity) {
        gameInRange = true;
        gameTargetPresent = true;
    } else if (type == off::HitResult::TypeEntityOutOfRange) {
        gameTargetPresent = true;
    }
}

void tick(void* rawPlayer) {
    if (!module || !module->enabled || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) {
        clearState();
        return;
    }
    auto* player = reinterpret_cast<bedrocktools::sdk::Actor*>(rawPlayer);
    const AABB playerBox = player->bounds();
    if (!hitrange::validBox(playerBox)) { clearState(); return; }

    const auto now = Clock::now();
    const float reach = hitrange::safeReach(module->reach);
    bool gameInRange = false, gameTargetPresent = false;
    readGameVerdict(rawPlayer, gameInRange, gameTargetPresent);

    // Copy the previous interpolation state, then sample the game outside the
    // lock so the render hook never waits on an actor fetch.
    hitbox::Track selfTrack{};
    Clock::time_point selfSampleTime{};
    Clock::time_point lastTick{};
    std::vector<TargetState> previousTargets;
    {
        std::lock_guard lock(stateMutex);
        if (trackedPlayer != rawPlayer) {
            state = {};
            trackedPlayer = rawPlayer;
        }
        selfTrack = state.selfTrack;
        selfSampleTime = state.selfSampleTime;
        lastTick = state.lastTick;
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
    if (hitbox::pushSample(selfTrack, hitrange::feetCenter(playerBox), selfElapsed))
        selfSampleTime = now;

    std::vector<TargetState> nextTargets;
    std::vector<AABB> enemyBoxes;
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
                enemyBoxes.push_back(box);
                if (nextTargets.size() >= maxTargetRings) continue; // keep checking for the verdict

                TargetState target;
                target.actor = actor;
                target.box = box;
                target.inRange = hitrange::inReach(playerBox, box, reach);
                const Vec3 center = hitrange::feetCenter(box);
                auto old = std::find_if(previousTargets.begin(), previousTargets.end(),
                    [actor](const TargetState& t) { return t.actor == actor; });
                if (old != previousTargets.end()) {
                    target.track = old->track;
                    target.sampleTime = old->sampleTime;
                    const float elapsed = target.sampleTime == Clock::time_point{}
                        ? 0.05f : std::chrono::duration<float>(now - target.sampleTime).count();
                    if (hitbox::pushSample(target.track, center, elapsed)) target.sampleTime = now;
                } else {
                    hitbox::pushSample(target.track, center, 0.05f);
                    target.sampleTime = now;
                }
                nextTargets.push_back(std::move(target));
            }
        }
    }
    const auto verdict = hitrange::classify(
        playerBox, enemyBoxes, reach, gameInRange, gameTargetPresent);

    {
        std::lock_guard lock(stateMutex);
        if (trackedPlayer != rawPlayer) return; // a newer tick owns a different player
        state.hasPlayer = true;
        state.targetPresent = verdict.targetPresent;
        state.inRange = verdict.inRange;
        state.selfTrack = selfTrack;
        state.selfSampleTime = selfSampleTime;
        state.lastTick = now;
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

    const float reach = hitrange::safeReach(module->reach);
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
    : Module("Hit Range", "Draws a red/green ground circle at your melee reach and around nearby targets.") {
    module = this;
    hideInHudEditor = true;
}
HitRangeModule::~HitRangeModule() { if (module == this) module = nullptr; }

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
    levelGetHitResult = reinterpret_cast<LevelGetHitResult>(resolve(SignatureId::LevelGetHitResult));
    fetchNearby = reinterpret_cast<FetchNearby>(resolve(SignatureId::ActorFetchNearbyActorsSorted));
    auto group = resolve(SignatureId::RenderMaterialGroupCommon);
    if (group) {
        auto address = resolveAdrp(reinterpret_cast<std::uint32_t*>(group), 2, 0);
        if (address) materialGroup = address + off::MaterialGroup::mRenderMaterialGroupOffset;
    }
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tick(event.player); });
}

void HitRangeModule::onEnable() {
    clearState();
    if (!m_patched && m_patchTarget)
        m_patched = bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(&renderHook),
                                                 reinterpret_cast<void**>(&originalRender)) != nullptr;
}
void HitRangeModule::onDisable() { clearState(); }

void HitRangeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("effectStyle") && j["effectStyle"].is_string())
        style = jumpcircle::styleIndex(j["effectStyle"].get<std::string>());
    if (j.contains("reach")) {
        try { reach = hitrange::safeReach(j["reach"].get<float>()); } catch (...) {}
    }
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
    j["reach"] = hitrange::safeReach(reach);
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
