#pragma once

// Pure geometry and configuration helpers for the Hit Range overlay.
//
// Green means "an eligible player/mob is inside the automatic 3/5-block 3D
// reach and at least one face visible from the player's eye has unobstructed
// sight lines". It is deliberately independent of the camera/crosshair
// direction: turning the screen must not turn a reachable target red or an
// unreachable one green. Sampling those visible faces also covers attacks from
// above or below the target by one or more blocks. It is not plain AABB
// proximity: a solid wall across every visible face keeps the ring red. The
// native BlockSource solid-block query supplies occlusion; without that
// signature the verdict fails closed to red, never to a crosshair-only guess.
// These helpers are host-testable without Minecraft.

#include "hitbox_interp.hpp"
#include "jumpcircle_geometry.hpp"
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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

inline constexpr int maxSightVoxels = 128;
// Short holds suppress hit-frame flashes without leaving stale green state.
inline constexpr double greenHoldSeconds = 0.18;
inline constexpr float maxCenterStep = 0.75f; // blocks per sampled tick
inline constexpr int modeConfirmations = 2;

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

inline bedrocktools::sdk::Vec3 boxCenter(const bedrocktools::sdk::AABB& box) {
    return {(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f};
}
inline bedrocktools::sdk::Vec3 feetCenter(const bedrocktools::sdk::AABB& box,
                                           float lift = groundLift) {
    return {(box.min.x + box.max.x) * 0.5f, box.min.y + lift,
            (box.min.z + box.max.z) * 0.5f};
}

// Bedrock measures melee reach from the eye, not from the feet or the AABB
// centre. The eye is clamped to the box height so sneaking/shrinking does not
// put the sight ray above the player's head.
inline bedrocktools::sdk::Vec3 eyePosition(const bedrocktools::sdk::AABB& box) {
    const float height = std::max(0.0f, box.max.y - box.min.y);
    const float eyeHeight = std::min(1.62f, height * 0.9f);
    return {(box.min.x + box.max.x) * 0.5f, box.min.y + eyeHeight,
            (box.min.z + box.max.z) * 0.5f};
}

inline float axisGap(float aMin, float aMax, float bMin, float bMax) {
    if (aMax < bMin) return bMin - aMax;
    if (bMax < aMin) return aMin - bMax;
    return 0.0f;
}

// Shortest distance between two collision boxes. Retained for diagnostics and
// host tests; the green/red decision uses eye-to-target-box distance and
// solid-block line of sight, never this box-to-box proximity alone.
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
inline float pointBoxDistanceSq(const bedrocktools::sdk::Vec3& point,
                                const bedrocktools::sdk::AABB& box) {
    if (!validBox(box)) return std::numeric_limits<float>::infinity();
    const float dx = axisGap(point.x, point.x, box.min.x, box.max.x);
    const float dy = axisGap(point.y, point.y, box.min.y, box.max.y);
    const float dz = axisGap(point.z, point.z, box.min.z, box.max.z);
    return dx * dx + dy * dy + dz * dz;
}

struct TargetCandidate {
    const void* actor = nullptr;
    bedrocktools::sdk::AABB box{};
};

// Pure callback interface for the native BlockSource solid-block query.
struct SightTest {
    using SolidBlock = bool (*)(const void* context, int x, int y, int z);
    const void* context = nullptr;
    SolidBlock isSolid = nullptr;
    explicit operator bool() const { return isSolid != nullptr; }
};

inline bool voxelIntersectsBox(int x, int y, int z,
                               const bedrocktools::sdk::AABB& box) {
    constexpr float epsilon = 1e-4f;
    const float fx = static_cast<float>(x), fy = static_cast<float>(y), fz = static_cast<float>(z);
    return fx + 1.0f > box.min.x + epsilon && fx < box.max.x - epsilon &&
           fy + 1.0f > box.min.y + epsilon && fy < box.max.y - epsilon &&
           fz + 1.0f > box.min.z + epsilon && fz < box.max.z - epsilon;
}

// Amanatides & Woo voxel traversal from eye to one visible point on the target.
// The starting voxel, the endpoint's voxel and any voxel overlapping the target
// AABB are never tested, so camera clipping, mobs hugging a wall and vertical
// angles do not mistake the target's own volume for an occluding wall.
inline bool sightRayHitsSolid(const bedrocktools::sdk::Vec3& origin,
                              const bedrocktools::sdk::Vec3& target,
                              const SightTest& sight,
                              const bedrocktools::sdk::AABB* ignoreBox = nullptr) {
    if (!sight || !hitbox::isFinite(origin) || !hitbox::isFinite(target)) return false;
    const float dx = target.x - origin.x;
    const float dy = target.y - origin.y;
    const float dz = target.z - origin.z;
    const float lengthSq = dx * dx + dy * dy + dz * dz;
    if (!(lengthSq > 1e-8f) || !std::isfinite(lengthSq)) return false;

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));
    const int ex = static_cast<int>(std::floor(target.x));
    const int ey = static_cast<int>(std::floor(target.y));
    const int ez = static_cast<int>(std::floor(target.z));
    if (x == ex && y == ey && z == ez) return false;

    int stepX = 0, stepY = 0, stepZ = 0;
    constexpr float inf = 1e30f;
    float tMaxX = inf, tMaxY = inf, tMaxZ = inf;
    float tDeltaX = inf, tDeltaY = inf, tDeltaZ = inf;
    if (dx > 0.0f) {
        stepX = 1; tDeltaX = 1.0f / dx; tMaxX = (x + 1 - origin.x) * tDeltaX;
    } else if (dx < 0.0f) {
        stepX = -1; tDeltaX = -1.0f / dx; tMaxX = (origin.x - x) * tDeltaX;
    }
    if (dy > 0.0f) {
        stepY = 1; tDeltaY = 1.0f / dy; tMaxY = (y + 1 - origin.y) * tDeltaY;
    } else if (dy < 0.0f) {
        stepY = -1; tDeltaY = -1.0f / dy; tMaxY = (origin.y - y) * tDeltaY;
    }
    if (dz > 0.0f) {
        stepZ = 1; tDeltaZ = 1.0f / dz; tMaxZ = (z + 1 - origin.z) * tDeltaZ;
    } else if (dz < 0.0f) {
        stepZ = -1; tDeltaZ = -1.0f / dz; tMaxZ = (origin.z - z) * tDeltaZ;
    }

    for (int step = 0; step < maxSightVoxels; ++step) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            if (tMaxX > 1.0f) break;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            if (tMaxY > 1.0f) break;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            if (tMaxZ > 1.0f) break;
            tMaxZ += tDeltaZ;
        }
        if (x == ex && y == ey && z == ez) break;
        if (ignoreBox && voxelIntersectsBox(x, y, z, *ignoreBox)) continue;
        if (sight.isSolid(sight.context, x, y, z)) return true;
    }
    return false;
}

// Build five inset sample points on one face of the target AABB. Problem 6
// showed that always aiming the LOS test at the box centre/head made targets
// above or below the player turn red: those rays cut through supporting/cage
// blocks. Testing complete faces instead covers vertical offsets while a wall
// covering any part of every visible face still stays red. Five samples per
// face preserve problem 5's rejection of partial visibility.
using FaceSamples = std::array<bedrocktools::sdk::Vec3, 5>;

inline FaceSamples samplesOnYFace(float faceY, float lowX, float highX,
                                  float lowZ, float highZ) {
    constexpr float q = 0.25f, r = 0.75f, h = 0.5f;
    return {{{std::lerp(lowX, highX, h), faceY, std::lerp(lowZ, highZ, h)},
             {std::lerp(lowX, highX, q), faceY, std::lerp(lowZ, highZ, q)},
             {std::lerp(lowX, highX, r), faceY, std::lerp(lowZ, highZ, q)},
             {std::lerp(lowX, highX, q), faceY, std::lerp(lowZ, highZ, r)},
             {std::lerp(lowX, highX, r), faceY, std::lerp(lowZ, highZ, r)}}};
}
inline FaceSamples samplesOnXFace(float faceX, float lowY, float highY,
                                  float lowZ, float highZ) {
    constexpr float q = 0.25f, r = 0.75f, h = 0.5f;
    return {{{faceX, std::lerp(lowY, highY, h), std::lerp(lowZ, highZ, h)},
             {faceX, std::lerp(lowY, highY, q), std::lerp(lowZ, highZ, q)},
             {faceX, std::lerp(lowY, highY, r), std::lerp(lowZ, highZ, q)},
             {faceX, std::lerp(lowY, highY, q), std::lerp(lowZ, highZ, r)},
             {faceX, std::lerp(lowY, highY, r), std::lerp(lowZ, highZ, r)}}};
}
inline FaceSamples samplesOnZFace(float faceZ, float lowX, float highX,
                                  float lowY, float highY) {
    constexpr float q = 0.25f, r = 0.75f, h = 0.5f;
    return {{{std::lerp(lowX, highX, h), std::lerp(lowY, highY, h), faceZ},
             {std::lerp(lowX, highX, q), std::lerp(lowY, highY, q), faceZ},
             {std::lerp(lowX, highX, r), std::lerp(lowY, highY, q), faceZ},
             {std::lerp(lowX, highX, q), std::lerp(lowY, highY, r), faceZ},
             {std::lerp(lowX, highX, r), std::lerp(lowY, highY, r), faceZ}}};
}

// Return one candidate face per axis the eye is outside. From above/below the
// player gets a top/bottom face in addition to the horizontal near face; an
// unobstructed face is enough to land a melee hit from that direction.
inline std::vector<FaceSamples> visibleFaceCandidates(
    const bedrocktools::sdk::Vec3& eye, const bedrocktools::sdk::AABB& box) {
    constexpr float maxInset = 0.05f;
    const float insetX = std::min(maxInset, (box.max.x - box.min.x) * 0.25f);
    const float insetY = std::min(maxInset, (box.max.y - box.min.y) * 0.25f);
    const float insetZ = std::min(maxInset, (box.max.z - box.min.z) * 0.25f);
    const float lowX = box.min.x + insetX, highX = box.max.x - insetX;
    const float lowY = box.min.y + insetY, highY = box.max.y - insetY;
    const float lowZ = box.min.z + insetZ, highZ = box.max.z - insetZ;
    std::vector<FaceSamples> faces;
    faces.reserve(3);
    if (eye.y > box.max.y) faces.push_back(samplesOnYFace(highY, lowX, highX, lowZ, highZ));
    else if (eye.y < box.min.y) faces.push_back(samplesOnYFace(lowY, lowX, highX, lowZ, highZ));
    if (eye.x > box.max.x) faces.push_back(samplesOnXFace(highX, lowY, highY, lowZ, highZ));
    else if (eye.x < box.min.x) faces.push_back(samplesOnXFace(lowX, lowY, highY, lowZ, highZ));
    if (eye.z > box.max.z) faces.push_back(samplesOnZFace(highZ, lowX, highX, lowY, highY));
    else if (eye.z < box.min.z) faces.push_back(samplesOnZFace(lowZ, lowX, highX, lowY, highY));
    if (faces.empty()) faces.push_back(samplesOnYFace(highY, lowX, highX, lowZ, highZ));
    return faces;
}

inline bool hasClearLineOfSight(const bedrocktools::sdk::Vec3& eye,
                                const bedrocktools::sdk::AABB& box,
                                const SightTest& sight) {
    if (!sight || !validBox(box)) return false;
    if (pointBoxDistanceSq(eye, box) <= 1e-8f) return true; // eye already inside target
    for (const auto& face : visibleFaceCandidates(eye, box)) {
        bool faceClear = true;
        for (const auto& point : face) {
            if (sightRayHitsSolid(eye, point, sight, &box)) { faceClear = false; break; }
        }
        if (faceClear) return true;
    }
    return false;
}

// Range is the 3D eye-to-nearest-point-on-target-box distance, not
// player-AABB-to-target-AABB or a flat horizontal radius. The green decision is
// camera-independent: it needs only the eye position and the native solid-block
// sight test, including targets above/below by up to the automatic reach. There
// is no frontal cone, no crosshair HitResult and no exact-aim fallback. Without
// the occlusion signature the module fails closed to red instead of promising
// a hit through an unseen wall.
inline bool candidateHittable(const TargetCandidate& candidate, float reach,
                              const bedrocktools::sdk::Vec3& eye,
                              const SightTest& sight) {
    if (!sight || !validBox(candidate.box)) return false;
    const float limit = safeReach(reach);
    if (pointBoxDistanceSq(eye, candidate.box) > limit * limit) return false;
    return hasClearLineOfSight(eye, candidate.box, sight);
}

struct ReachVerdict {
    bool targetPresent = false;
    bool inRange = false;
    const void* hittableActor = nullptr; // first qualifying actor, for diagnostics
    std::vector<const void*> hittableActors{};
};

// `eye` is the only player-state input. Camera rotation/crosshair hit results
// are intentionally absent: rotating the screen must not change green/red.
inline ReachVerdict classify(const std::vector<TargetCandidate>& candidates,
                             float reach, const bedrocktools::sdk::Vec3& eye,
                             const SightTest& sight = {}) {
    ReachVerdict verdict;
    for (const auto& candidate : candidates) {
        verdict.targetPresent = true;
        if (candidateHittable(candidate, reach, eye, sight))
            verdict.hittableActors.push_back(candidate.actor);
    }
    verdict.inRange = !verdict.hittableActors.empty();
    verdict.hittableActor = verdict.inRange ? verdict.hittableActors.front() : nullptr;
    return verdict;
}

// Green enters immediately but leaves only after a short dwell, so a one-frame
// actor-fetch miss, arm swing or occlusion read cannot flash the rings red.
struct VerdictHold {
    bool initialized = false;
    bool stable = false;
    double changedAt = 0.0;

    bool update(bool candidate, double nowSeconds,
                double holdSeconds = greenHoldSeconds) {
        if (!std::isfinite(nowSeconds) || !std::isfinite(holdSeconds)) return stable;
        if (nowSeconds < changedAt) changedAt = nowSeconds;
        if (!initialized) {
            initialized = true;
            stable = candidate;
            changedAt = nowSeconds;
            return stable;
        }
        if (candidate != stable) {
            const double required = candidate ? 0.0 : std::max(0.0, holdSeconds);
            if (nowSeconds - changedAt >= required) {
                stable = candidate;
                changedAt = nowSeconds;
            }
        } else if (candidate) {
            changedAt = nowSeconds; // refresh the green hold while still valid
        }
        return stable;
    }
};

// A single wrong attack/mode-event label must not resize every ring from 3 to
// 5 blocks. Confirmed live game-mode reads bypass the filter; weaker events
// need two consistent samples.
struct StableMode {
    bool creative = false;
    int pending = -1;
    int streak = 0;

    void reset(bool value) {
        creative = value;
        pending = -1;
        streak = 0;
    }
    bool observe(bool desired, int confirmations = modeConfirmations) {
        confirmations = std::max(1, confirmations);
        if (desired == creative) {
            pending = -1;
            streak = 0;
            return creative;
        }
        const int value = desired ? 1 : 0;
        if (pending != value) {
            pending = value;
            streak = 1;
        } else {
            ++streak;
        }
        if (streak >= confirmations) {
            creative = desired;
            pending = -1;
            streak = 0;
        }
        return creative;
    }
};

// Clamp a ring centre's per-tick movement. Knockback or a recycled actor read
// cannot make the circle teleport; it catches up smoothly over later ticks.
inline bedrocktools::sdk::Vec3 stepToward(const bedrocktools::sdk::Vec3& from,
                                          const bedrocktools::sdk::Vec3& to,
                                          float maxDistance) {
    if (!hitbox::isFinite(from) || !hitbox::isFinite(to) ||
        !(maxDistance > 0.0f) || !std::isfinite(maxDistance)) return from;
    const bedrocktools::sdk::Vec3 delta{to.x - from.x, to.y - from.y, to.z - from.z};
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                     delta.z * delta.z);
    if (!(distance > maxDistance)) return to;
    const float scale = maxDistance / distance;
    return {from.x + delta.x * scale, from.y + delta.y * scale,
            from.z + delta.z * scale};
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
