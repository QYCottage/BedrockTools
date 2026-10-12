#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
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

void applyAimAssistTurnDelta(bedrocktools::sdk::Vec2& delta) {
    if (g_aimAssist) g_aimAssist->applyTurnDelta(delta);

bool hasWeaponInMainHand(const bedrocktools::sdk::Player* player) {
    if (!player) return false;

    auto** vtable = *reinterpret_cast<void***>(
        const_cast<bedrocktools::sdk::Player*>(player));
    constexpr std::size_t getCarriedItemIndex =
        bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[getCarriedItemIndex]) return false;

    using GetCarriedItemFn = const void* (*)(void*);
    const void* stack = reinterpret_cast<GetCarriedItemFn>(
        vtable[getCarriedItemIndex])(const_cast<bedrocktools::sdk::Player*>(player));
    if (!stack) return false;

    const auto& inventory = bedrocktools::core::InventoryAccess::get();
    constexpr const char* weaponIdentifiers[] = {
        "_sword", "_axe", "mace", "trident", "spear", "bow",
        "dagger", "katana", "rapier", "scythe", "hammer",
        "halberd", "glaive", "lance"
    };
    for (const char* identifier : weaponIdentifiers) {
        if (inventory.countStack(stack, identifier) > 0) return true;
    }
    return false;
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
    resetCameraState();
}

void AimAssistModule::onDisable() {
    resetCameraState();
}

void AimAssistModule::clearTargetState() {
    m_lastTarget = nullptr;
    m_pendingYawError.store(0.0f, std::memory_order_relaxed);
    m_pendingPitchError.store(0.0f, std::memory_order_relaxed);
    m_pendingStrength.store(0.0f, std::memory_order_relaxed);
}

void AimAssistModule::resetCameraState() {
    clearTargetState();
    m_cameraPitch.store(0.0f, std::memory_order_relaxed);
    m_cameraYaw.store(0.0f, std::memory_order_relaxed);
    m_cameraRotationInitialized.store(false, std::memory_order_release);
}

void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs) ||
        (m_weaponOnly && !hasWeaponInMainHand(player))) {
        clearTargetState();
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    auto* level = player->level();
    if (!rotationComponent || !level) {
        clearTargetState();
        return;
    }

    const Vec2 actorRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(actorRotation.x) || !std::isfinite(actorRotation.y)) {
        clearTargetState();
        return;
    }
    if (!m_cameraRotationInitialized.load(std::memory_order_acquire)) {
        m_cameraPitch.store(std::clamp(actorRotation.x, -89.0f, 89.0f), std::memory_order_relaxed);
        m_cameraYaw.store(wrapDegrees(actorRotation.y), std::memory_order_relaxed);
        m_cameraRotationInitialized.store(true, std::memory_order_release);
    }
    Vec2 currentRotation{
        m_cameraPitch.load(std::memory_order_relaxed),
        m_cameraYaw.load(std::memory_order_relaxed)
    };
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        clearTargetState();
        return;
    }

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        clearTargetState();
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
        clearTargetState();
        return;
    }

    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    if (!actors.begin || !actors.end || actors.end < actors.begin) {
        clearTargetState();
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
    if (!bestTarget) {
        clearTargetState();
        return;
    }

    m_pendingYawError.store(wrapDegrees(bestRotation.y - currentRotation.y), std::memory_order_relaxed);
    m_pendingPitchError.store(bestRotation.x - currentRotation.x, std::memory_order_relaxed);
    m_pendingFov.store(maxFov, std::memory_order_relaxed);
    m_pendingStrength.store(std::clamp(m_strength, 0.01f, 1.0f), std::memory_order_relaxed);
}

void AimAssistModule::applyTurnDelta(bedrocktools::sdk::Vec2& delta) {
    if (!enabled || !std::isfinite(delta.x) || !std::isfinite(delta.y)) return;
    if (!m_cameraRotationInitialized.load(std::memory_order_acquire)) return;

    const float fov = std::clamp(m_pendingFov.load(std::memory_order_relaxed), 1.0f, 90.0f);
    const float strength = std::clamp(m_pendingStrength.load(std::memory_order_relaxed), 0.0f, 0.5f);
    const float yawError = m_pendingYawError.load(std::memory_order_relaxed);
    const float pitchError = m_pendingPitchError.load(std::memory_order_relaxed);
    const float inputMagnitude = std::hypot(delta.x, delta.y);

    // Assist only while the player is already moving the camera; no idle auto-lock.
    if (strength > 0.0f && std::isfinite(inputMagnitude) && inputMagnitude > 0.0001f
        && std::isfinite(yawError) && std::isfinite(pitchError)) {
        const float yawWeight = std::clamp(yawError / fov, -1.0f, 1.0f);
        const float pitchWeight = std::clamp(pitchError / fov, -1.0f, 1.0f);
        delta.y += yawWeight * inputMagnitude * strength;
        delta.x += pitchWeight * inputMagnitude * strength;
    }

    // Track the camera-input stream locally instead of writing to the actor rotation.
    const float pitch = m_cameraPitch.load(std::memory_order_relaxed);
    const float yaw = m_cameraYaw.load(std::memory_order_relaxed);
    m_cameraPitch.store(std::clamp(pitch + delta.x, -89.0f, 89.0f), std::memory_order_relaxed);
    m_cameraYaw.store(wrapDegrees(yaw + delta.y), std::memory_order_relaxed);
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_weaponOnly = j.value("m_weaponOnly", m_weaponOnly);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 12.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 90.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.01f, 1.0f);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_weaponOnly"] = m_weaponOnly;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_strength"] = m_strength;
}
