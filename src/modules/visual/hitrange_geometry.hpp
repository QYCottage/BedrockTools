#pragma once

// Pure geometry and configuration helpers for the Hit Range overlay.
//
// The reference effect is a thin ground ring at melee reach: red while no
// enemy can be hit, green as soon as one is inside the reach. Keeping the
// reach test, color parsing and the ring mesh here makes them testable on the
// host without Minecraft, signatures or the Android toolchain.

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

inline constexpr float defaultReach = 3.0f;   // survival melee reach
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

inline float axisGap(float aMin, float aMax, float bMin, float bMax) {
    if (aMax < bMin) return bMin - aMax;
    if (bMax < aMin) return aMin - bMax;
    return 0.0f;
}

// Shortest distance between two collision boxes. Using the boxes rather than
// their centres keeps large mobs and players at different heights from being
// reported out of reach while their hitboxes are still close enough to hit.
inline float boxDistanceSq(const bedrocktools::sdk::AABB& a,
                           const bedrocktools::sdk::AABB& b) {
    const float dx = axisGap(a.min.x, a.max.x, b.min.x, b.max.x);
    const float dy = axisGap(a.min.y, a.max.y, b.min.y, b.max.y);
    const float dz = axisGap(a.min.z, a.max.z, b.min.z, b.max.z);
    return dx * dx + dy * dy + dz * dz;
}
inline bool inReach(const bedrocktools::sdk::AABB& player,
                    const bedrocktools::sdk::AABB& target, float reach) {
    const float limit = safeReach(reach);
    return boxDistanceSq(player, target) <= limit * limit;
}

struct ReachVerdict {
    bool targetPresent = false;
    bool inRange = false;
};

// `gameInRange` is the game's own crosshair verdict (HitResult::TypeEntity).
// It is ORed with the geometric test so the ring also agrees with creative
// reach and item reach bonuses that are not represented by the slider.
inline ReachVerdict classify(const bedrocktools::sdk::AABB& player,
                             const std::vector<bedrocktools::sdk::AABB>& enemies,
                             float reach, bool gameInRange = false,
                             bool gameTargetPresent = false) {
    ReachVerdict verdict{gameTargetPresent || gameInRange, gameInRange};
    for (const auto& enemy : enemies) {
        verdict.targetPresent = true;
        if (inReach(player, enemy, reach)) verdict.inRange = true;
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
