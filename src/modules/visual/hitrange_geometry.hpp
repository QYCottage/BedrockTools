#pragma once

// Pure geometry and configuration helpers for the Hit Range overlay.
//
// The reference effect is a thin ground ring at melee reach. Green must mean
// "the game would actually let me hit the entity under the crosshair", not
// merely "some entity happens to be within three blocks of my collision box".
// The authoritative verdict comes from Level::getHitResult(); the ray/AABB
// helpers below are only a fallback for builds where that signature is
// missing. Keeping them here makes the reach test testable on the host.

#include "hitbox_interp.hpp"
#include "jumpcircle_geometry.hpp"
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace hitrange {

inline constexpr float survivalReach = 3.0f;
inline constexpr float creativeReach = 5.0f;
inline constexpr float defaultReach = survivalReach;
inline constexpr float minReach = 1.0f;
inline constexpr float maxReach = 8.0f;       // covers creative reach (5)
inline constexpr float defaultWidth = 0.10f; // blocks
inline constexpr float minWidth = 0.02f;
inline constexpr float maxWidth = 0.50f;
inline constexpr float groundLift = 0.03f;   // avoids z-fighting with the floor
inline constexpr int segments = jumpcircle::segments;
inline constexpr int bands = jumpcircle::bands;
inline constexpr int vertexCount = segments * bands * 4;

inline constexpr std::uint32_t defaultInRangeColor = 0xFF35D07Fu;    // green
inline constexpr std::uint32_t defaultOutOfRangeColor = 0xFFFF453Au; // red

inline float safeReach(float value) {
    return std::isfinite(value) ? std::clamp(value, minReach, maxReach) : defaultReach;
}
inline float safeWidth(float value) {
    return std::isfinite(value) ? std::clamp(value, minWidth, maxWidth) : defaultWidth;
}

// The launcher color picker sends #AARRGGBB; #RRGGBB is accepted for configs
// written by hand. Anything else keeps the caller's current color.
inline std::uint32_t parseColor(std::string_view value, std::uint32_t fallback) {
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return fallback;
    if (value.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string_view::npos) return fallback;
    try {
        const auto parsed = static_cast<std::uint32_t>(
            std::stoul(std::string(value.substr(1)), nullptr, 16));
        return value.size() == 7 ? (0xFF000000u | parsed) : parsed;
    } catch (...) {
        return fallback;
    }
}
inline std::string colorString(std::uint32_t color) {
    char text[10];
    std::snprintf(text, sizeof(text), "#%08X", color);
    return text;
}

inline bool validBox(const bedrocktools::sdk::AABB& box) {
    const auto finite = [](const bedrocktools::sdk::Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    return finite(box.min) && finite(box.max) &&
           box.max.x >= box.min.x && box.max.y >= box.min.y && box.max.z >= box.min.z;
}

inline bedrocktools::sdk::Vec3 feetCenter(const bedrocktools::sdk::AABB& box,
                                           float lift = groundLift) {
    return {(box.min.x + box.max.x) * 0.5f, box.min.y + lift,
            (box.min.z + box.max.z) * 0.5f};
}

// Bedrock measures melee reach from the eye, not from the feet or the AABB
// centre. The eye is clamped to the box height so sneaking/shrinking does not
// put the fallback ray above the player's head.
inline bedrocktools::sdk::Vec3 eyePosition(const bedrocktools::sdk::AABB& box) {
    const float height = std::max(0.0f, box.max.y - box.min.y);
    const float eyeHeight = std::min(1.62f, height * 0.9f);
    return {(box.min.x + box.max.x) * 0.5f, box.min.y + eyeHeight,
            (box.min.z + box.max.z) * 0.5f};
}

// Rotation is (pitch, yaw) in degrees, matching Actor::rotation(). The result
// is a unit vector in world space, using the same convention as the Hitbox
// indicator's look-ray fallback.
inline bedrocktools::sdk::Vec3 lookVector(bedrocktools::sdk::Vec2 rotation) {
    constexpr float pi = 3.14159265358979323846f;
    constexpr float degToRad = pi / 180.0f;
    const float yaw = rotation.y * degToRad;
    const float pitch = rotation.x * degToRad;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
            std::cos(yaw) * std::cos(pitch)};
}

inline float axisGap(float aMin, float aMax, float bMin, float bMax) {
    if (aMax < bMin) return bMin - aMax;
    if (bMax < aMin) return aMin - bMax;
    return 0.0f;
}

// Shortest distance between two collision boxes. Retained for diagnostics and
// host tests; it is deliberately NOT used to decide the green/red state any
// more, because a nearby but unaimed/occluded entity is not attackable.
inline float boxDistanceSq(const bedrocktools::sdk::AABB& a,
                           const bedrocktools::sdk::AABB& b) {
    const float dx = axisGap(a.min.x, a.max.x, b.min.x, b.max.x);
    const float dy = axisGap(a.min.y, a.max.y, b.min.y, b.max.y);
    const float dz = axisGap(a.min.z, a.max.z, b.min.z, b.max.z);
    return dx * dx + dy * dy + dz * dz;
}
inline bool boxesWithin(const bedrocktools::sdk::AABB& player,
                        const bedrocktools::sdk::AABB& target, float reach) {
    const float limit = safeReach(reach);
    return boxDistanceSq(player, target) <= limit * limit;
}

// Slab ray/AABB intersection. `direction` must be normalized; a zero-length
// direction is treated as a miss. Only a hit inside [0, maxDistance] counts.
inline bool rayHitsAABB(const bedrocktools::sdk::Vec3& origin,
                        const bedrocktools::sdk::Vec3& direction,
                        const bedrocktools::sdk::AABB& box, float maxDistance) {
    if (!validBox(box) || !(maxDistance > 0.0f)) return false;
    const float lengthSq = direction.x * direction.x + direction.y * direction.y +
                           direction.z * direction.z;
    if (!(lengthSq > 0.0f) || !std::isfinite(lengthSq)) return false;
    float tMin = 0.0f;
    float tMax = maxDistance;
    const float o[3]{origin.x, origin.y, origin.z};
    const float d[3]{direction.x, direction.y, direction.z};
    const float lo[3]{box.min.x, box.min.y, box.min.z};
    const float hi[3]{box.max.x, box.max.y, box.max.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
            continue;
        }
        float t1 = (lo[axis] - o[axis]) / d[axis];
        float t2 = (hi[axis] - o[axis]) / d[axis];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return false;
    }
    return true;
}

enum class GameHit : std::uint8_t {
    Unknown,          // signature missing or unreadable hit result
    Entity,           // HitResult::TypeEntity: attackable entity under crosshair
    EntityOutOfRange, // HitResult::TypeEntityOutOfRange
    Other,            // block / no hit / any other type
};

struct GameVerdict {
    bool available = false;
    GameHit hit = GameHit::Unknown;
    const void* entity = nullptr; // never dereferenced, only compared
};

struct PlayerView {
    bedrocktools::sdk::Vec3 eye{};
    bedrocktools::sdk::Vec3 look{}; // normalized
};

struct ReachVerdict {
    bool targetPresent = false;
    bool inRange = false;
    const void* hittableActor = nullptr; // entity to draw green, when known
};

// The game's crosshair verdict is authoritative when it is available: green
// only for TypeEntity, red for TypeEntityOutOfRange or a block/no-hit. ORing a
// geometric "enemy nearby" test on top of that produced false greens in the
// reported bug (entity behind a wall, or not actually under the crosshair).
// The eye-ray fallback is used only when the hit-result signature is missing.
inline ReachVerdict classify(const std::vector<bedrocktools::sdk::AABB>& enemies,
                             float reach, const GameVerdict& game,
                             const PlayerView& view) {
    if (game.available) {
        switch (game.hit) {
            case GameHit::Entity:
                return {true, true, game.entity};
            case GameHit::EntityOutOfRange:
                return {true, false, nullptr};
            default:
                return {false, false, nullptr};
        }
    }
    ReachVerdict verdict;
    const float limit = safeReach(reach);
    for (const auto& enemy : enemies) {
        verdict.targetPresent = true;
        if (rayHitsAABB(view.eye, view.look, enemy, limit)) verdict.inRange = true;
    }
    return verdict;
}

inline bedrocktools::sdk::Vec3 interpolatedCenter(const hitbox::Track& track,
                                                   float elapsedSeconds) {
    const auto offset = hitbox::sampleOffset(
        track.prevCenter, track.curCenter,
        hitbox::sampleFraction(elapsedSeconds, track.intervalSeconds));
    return {track.curCenter.x + offset.x, track.curCenter.y + offset.y,
            track.curCenter.z + offset.z};
}

// One fixed-radius ring made of four concentric strips. The middle strip is
// fully opaque and both edges fade out, so the circle stays readable against
// bright sand and dark stone without needing a texture.
template <class Emit>
void emitRing(const bedrocktools::sdk::Vec3& center, float radius, float width,
              jumpcircle::Style style, Emit emit) {
    constexpr std::array<float, bands + 1> offsets{-0.5f, -0.2f, 0.0f, 0.2f, 0.5f};
    constexpr std::array<float, bands + 1> strengths{0.0f, 0.55f, 1.0f, 0.55f, 0.0f};
    const float r = std::max(0.0f, radius);
    const float w = safeWidth(width);
    for (int i = 0; i < segments; ++i) {
        const auto a = jumpcircle::contour(style, i);
        const auto b = jumpcircle::contour(style, i + 1);
        for (int band = 0; band < bands; ++band) {
            const float inner = std::max(0.0f, r + offsets[band] * w);
            const float outer = std::max(0.0f, r + offsets[band + 1] * w);
            const float alphaInner = strengths[band];
            const float alphaOuter = strengths[band + 1];
            emit(bedrocktools::sdk::Vec3{center.x + a.x * inner, center.y, center.z + a.z * inner}, alphaInner);
            emit(bedrocktools::sdk::Vec3{center.x + b.x * inner, center.y, center.z + b.z * inner}, alphaInner);
            emit(bedrocktools::sdk::Vec3{center.x + b.x * outer, center.y, center.z + b.z * outer}, alphaOuter);
            emit(bedrocktools::sdk::Vec3{center.x + a.x * outer, center.y, center.z + a.z * outer}, alphaOuter);
        }
    }
}

} // namespace hitrange
