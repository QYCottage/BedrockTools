#include "aimassist.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);

struct DistanceSortedActor {
    void* actor;
    float distance;
    float padding;
};

struct ActorVector {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* capacity;
};

using ActorFetchNearbyActorsSortedFn = ActorVector (*)(void*, void*, int);

AimAssistModule* g_aimAssist = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorIsInvisibleFn g_actorIsInvisible = nullptr;
ActorFetchNearbyActorsSortedFn g_fetchNearbyActors = nullptr;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

float wrapDegrees(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

bool finiteVec3(const bedrocktools::sdk::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    if (!finiteVec3(bounds.min) || !finiteVec3(bounds.max)) return false;
    const float height = bounds.max.y - bounds.min.y;
    const float width = bounds.max.x - bounds.min.x;
    const float depth = bounds.max.z - bounds.min.z;
    return height >= 0.25f && height <= 8.0f && width > 0.0f && width <= 5.0f
        && depth > 0.0f && depth <= 5.0f;
}

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool categoryHasMobBit(void* actor) {
    if (!actor) return false;
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mCategories);
    // BedrockTools' existing Hitbox module uses category bit 2 to identify entities.
    return (categories & 2u) != 0;
}
}

AimAssistModule::AimAssistModule()
    : Module("Aim Assist", "Smooth target tracking for private PvP training worlds.") {
    keybind = 0;
    g_aimAssist = this;
}

AimAssistModule::~AimAssistModule() {
    if (g_aimAssist == this) g_aimAssist = nullptr;
}

void AimAssistModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_aimAssist && g_aimAssist->enabled) {
                g_aimAssist->processPlayerTick(event.player);
            }
        });
}

void AimAssistModule::onEnable() {
    m_lastTarget = nullptr;
}

void AimAssistModule::onDisable() {
    m_lastTarget = nullptr;
}

void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        m_lastTarget = nullptr;
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    auto* level = player->level();
    if (!rotationComponent || !level) {
        m_lastTarget = nullptr;
        return;
    }

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        m_lastTarget = nullptr;
        return;
    }

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        m_lastTarget = nullptr;
        return;
    }

    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    Vec3 extent{m_range, m_range, m_range};
    void* actorManager = field<void*>(level, offsets::Level::mActorManager);
    if (!actorManager) {
        m_lastTarget = nullptr;
        return;
    }

    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    if (!actors.begin || !actors.end || actors.end < actors.begin) {
        m_lastTarget = nullptr;
        return;
    }

    void* bestTarget = nullptr;
    Vec2 bestRotation{};
    float bestAngleScore = std::numeric_limits<float>::max();
    const float maxDistanceSq = m_range * m_range;
    const float maxFov = std::clamp(m_fov, 1.0f, 90.0f);

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        void* candidate = entry->actor;
        if (!candidate || candidate == player) continue;

        const bool isPlayer = g_actorIsPlayer && g_actorIsPlayer(candidate);
        if (isPlayer) {
            if (!m_targetPlayers) continue;
        } else {
            if (!m_targetMobs || !categoryHasMobBit(candidate)) continue;
        }

        if (g_actorIsInvisible && g_actorIsInvisible(candidate)) continue;

        // Actor is a lightweight SDK wrapper; no ownership or memory is transferred.
        auto* target = reinterpret_cast<Actor*>(candidate);
        const Vec3 targetPosition = target->position();
        const AABB targetBounds = target->bounds();
        if (!finiteVec3(targetPosition) || !validBounds(targetBounds)) continue;

        Vec3 aimPoint{};
        aimPoint.x = (targetBounds.min.x + targetBounds.max.x) * 0.5f;
        aimPoint.y = targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * 0.78f;
        aimPoint.z = (targetBounds.min.z + targetBounds.max.z) * 0.5f;

        const float d2 = distanceSquared(eyePosition, aimPoint);
        if (!std::isfinite(d2) || d2 > maxDistanceSq || d2 < 0.01f) continue;

        const float dx = aimPoint.x - eyePosition.x;
        const float dy = aimPoint.y - eyePosition.y;
        const float dz = aimPoint.z - eyePosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(-dx, dz) * kRadToDeg;
        const float desiredPitch = -std::atan2(dy, std::max(horizontal, 0.001f)) * kRadToDeg;

        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore) || angleScore > maxFov) continue;

        // Prefer the target closest to the crosshair, using distance only as a tiebreaker.
        const float targetStickiness = candidate == m_lastTarget ? 0.0f : 2.0f;
        const float score = angleScore + std::sqrt(d2) * 0.001f + targetStickiness;
        if (score < bestAngleScore) {
            bestAngleScore = score;
            bestTarget = candidate;
            bestRotation = {desiredPitch, desiredYaw};
        }
    }

    m_lastTarget = bestTarget;
    if (!bestTarget) return;

    const float strength = std::clamp(m_strength, 0.01f, 1.0f);
    const float yawDelta = wrapDegrees(bestRotation.y - currentRotation.y);
    const float pitchDelta = bestRotation.x - currentRotation.x;

    Vec2 updatedRotation{};
    updatedRotation.y = wrapDegrees(currentRotation.y + yawDelta * strength);
    updatedRotation.x = std::clamp(currentRotation.x + pitchDelta * strength, -89.0f, 89.0f);

    // The SDK already reads this Vec2 from the actor rotation component. Write only
    // these two floats, and only after a nearby valid training target has been selected.
    field<Vec2>(rotationComponent, 0) = updatedRotation;
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 12.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 90.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.01f, 1.0f);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_strength"] = m_strength;
}
