#pragma once

// Pure geometry and configuration helpers for the Hit Range overlay.
//
// Green means "an eligible player/mob is inside the automatic 3/5-block 3D
// reach and at least one exposed point on a face visible from the player's eye
// has an unobstructed sight line within that reach". It is deliberately
// independent of the camera/crosshair direction: turning the screen must not
// turn a reachable target red or an unreachable one green. Dense sampling of
// every near-facing face covers attacks from above, below, beside and behind;
// one clear, in-reach hit point is enough when a cage or support block hides
// only part of the target. It is not plain AABB proximity: a solid wall across
// every exposed point keeps the ring red. The native BlockSource solid-block
// query supplies occlusion; without that signature the verdict fails closed to
// red, never to a crosshair-only guess.
// These helpers are host-testable without Minecraft.

#include "hitbox_interp.hpp"
#include "jumpcircle_geometry.hpp"
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

inline float pointDistanceSq(const bedrocktools::sdk::Vec3& a,
                              const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
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

// Problem 7 showed that requiring one *complete* clear face made a same-level
// target behind a cage/support block red, while stepping one block above it
// exposed a full top face. A melee hit only needs one exposed point, so each
// near-facing face now contributes its closest point to the eye (preserving the
// exact in-reach corner) plus a 5x5 grid. The first clear, in-reach sample
// wins; a wall that blocks every exposed sample still keeps the ring red.
inline constexpr std::size_t faceGrid = 5;
inline constexpr std::size_t faceSampleCount = faceGrid * faceGrid + 1;
using FaceSamples = std::array<bedrocktools::sdk::Vec3, faceSampleCount>;
inline constexpr std::array<float, faceGrid> faceFractions{
    0.1f, 0.3f, 0.5f, 0.7f, 0.9f};

inline FaceSamples samplesOnYFace(const bedrocktools::sdk::Vec3& eye, float faceY,
                                  float lowX, float highX, float lowZ, float highZ) {
    FaceSamples samples{};
    samples[0] = {std::clamp(eye.x, lowX, highX), faceY,
                  std::clamp(eye.z, lowZ, highZ)};
    std::size_t index = 1;
    for (const float fz : faceFractions) {
        for (const float fx : faceFractions) {
            samples[index++] = {std::lerp(lowX, highX, fx), faceY,
                                std::lerp(lowZ, highZ, fz)};
        }
    }
    return samples;
}
inline FaceSamples samplesOnXFace(const bedrocktools::sdk::Vec3& eye, float faceX,
                                  float lowY, float highY, float lowZ, float highZ) {
    FaceSamples samples{};
    samples[0] = {faceX, std::clamp(eye.y, lowY, highY),
                  std::clamp(eye.z, lowZ, highZ)};
    std::size_t index = 1;
    for (const float fy : faceFractions) {
        for (const float fz : faceFractions) {
            samples[index++] = {faceX, std::lerp(lowY, highY, fy),
                                std::lerp(lowZ, highZ, fz)};
        }
    }
    return samples;
}
inline FaceSamples samplesOnZFace(const bedrocktools::sdk::Vec3& eye, float faceZ,
                                  float lowX, float highX, float lowY, float highY) {
    FaceSamples samples{};
    samples[0] = {std::clamp(eye.x, lowX, highX),
                  std::clamp(eye.y, lowY, highY), faceZ};
    std::size_t index = 1;
    for (const float fy : faceFractions) {
        for (const float fx : faceFractions) {
            samples[index++] = {std::lerp(lowX, highX, fx),
                                std::lerp(lowY, highY, fy), faceZ};
        }
    }
    return samples;
}

// Return one candidate face per axis the eye is outside. From above/below the
// player gets a top/bottom face in addition to the horizontal near face; every
// face is a set of possible first-hit points, not an all-or-nothing surface.
inline std::vector<FaceSamples> visibleFaceCandidates(
    const bedrocktools::sdk::Vec3& eye, const bedrocktools::sdk::AABB& box) {
    constexpr float maxInset = 0.001f; // keep exact reach edges from being lost to the inset
    const float insetX = std::min(maxInset, (box.max.x - box.min.x) * 0.2f);
    const float insetY = std::min(maxInset, (box.max.y - box.min.y) * 0.2f);
    const float insetZ = std::min(maxInset, (box.max.z - box.min.z) * 0.2f);
    const float lowX = box.min.x + insetX, highX = box.max.x - insetX;
    const float lowY = box.min.y + insetY, highY = box.max.y - insetY;
    const float lowZ = box.min.z + insetZ, highZ = box.max.z - insetZ;
    std::vector<FaceSamples> faces;
    faces.reserve(3);
    if (eye.y > box.max.y) faces.push_back(samplesOnYFace(eye, highY, lowX, highX, lowZ, highZ));
    else if (eye.y < box.min.y) faces.push_back(samplesOnYFace(eye, lowY, lowX, highX, lowZ, highZ));
    if (eye.x > box.max.x) faces.push_back(samplesOnXFace(eye, highX, lowY, highY, lowZ, highZ));
    else if (eye.x < box.min.x) faces.push_back(samplesOnXFace(eye, lowX, lowY, highY, lowZ, highZ));
    if (eye.z > box.max.z) faces.push_back(samplesOnZFace(eye, highZ, lowX, highX, lowY, highY));
    else if (eye.z < box.min.z) faces.push_back(samplesOnZFace(eye, lowZ, lowX, highX, lowY, highY));
    if (faces.empty()) faces.push_back(samplesOnYFace(eye, highY, lowX, highX, lowZ, highZ));
    return faces;
}

inline bool hasClearLineOfSight(const bedrocktools::sdk::Vec3& eye,
                                const bedrocktools::sdk::AABB& box,
                                float reach, const SightTest& sight) {
    if (!sight || !validBox(box) || !hitbox::isFinite(eye)) return false;
    const float limit = safeReach(reach);
    const float limitSq = limit * limit;
    const float nearestSq = pointBoxDistanceSq(eye, box);
    if (!std::isfinite(nearestSq) || nearestSq > limitSq) return false;
    if (nearestSq <= 1e-8f) return true; // eye already inside the target
    for (const auto& face : visibleFaceCandidates(eye, box)) {
        for (const auto& point : face) {
            // Reach is measured to the actual exposed hit point, not merely to
            // the nearest corner of a partly unreachable collision box.
            if (pointDistanceSq(eye, point) > limitSq) continue;
            if (!sightRayHitsSolid(eye, point, sight, &box)) return true;
        }
    }
    return false;
}

// Range is the 3D eye-to-exposed-hit-point distance, not player-AABB-to-target-
// AABB or a flat horizontal radius. The green decision is camera-independent:
// it needs only the eye position and the native solid-block sight test, works
// from every direction including above/below, and accepts partial visibility.
// There is no frontal cone, no crosshair HitResult and no exact-aim fallback.
// Without the occlusion signature the module fails closed to red instead of
// promising a hit through an unseen wall.
inline bool candidateHittable(const TargetCandidate& candidate, float reach,
                              const bedrocktools::sdk::Vec3& eye,
                              const SightTest& sight) {
    if (!sight || !validBox(candidate.box)) return false;
    return hasClearLineOfSight(eye, candidate.box, reach, sight);
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
