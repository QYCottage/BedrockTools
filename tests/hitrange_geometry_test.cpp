#include "modules/visual/hitrange_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    using namespace hitrange;
    using bedrocktools::sdk::AABB;
    using bedrocktools::sdk::Vec2;
    using bedrocktools::sdk::Vec3;

    assert(survivalReach == 3.0f && creativeReach == 5.0f);
    assert(safeReach(3.0f) == 3.0f);
    assert(safeReach(-10.0f) == minReach);
    assert(safeReach(100.0f) == maxReach);
    assert(safeReach(NAN) == defaultReach);
    assert(safeWidth(0.10f) == 0.10f);
    assert(safeWidth(0.0f) == minWidth);
    assert(safeWidth(9.0f) == maxWidth);
    assert(safeWidth(NAN) == defaultWidth);

    assert(parseColor("#80123456", 0) == 0x80123456u);
    assert(parseColor("#123456", 0) == 0xFF123456u);
    assert(parseColor("#8012345", 0xAABBCCDDu) == 0xAABBCCDDu);
    assert(parseColor("#8012345G", 0xAABBCCDDu) == 0xAABBCCDDu);
    assert(parseColor("80123456", 0xAABBCCDDu) == 0xAABBCCDDu);
    assert(colorString(0x80123456u) == "#80123456");
    assert(parseColor(colorString(0x80123456u), 0) == 0x80123456u);

    const AABB player{{-0.3f, 64.0f, -0.3f}, {0.3f, 65.8f, 0.3f}};
    const AABB nearSide{{2.0f, 64.0f, -0.3f}, {2.6f, 65.8f, 0.3f}};
    const AABB nearFront{{-0.3f, 64.0f, 2.0f}, {0.3f, 65.8f, 2.6f}};
    const AABB nearBehind{{-0.3f, 64.0f, -2.6f}, {0.3f, 65.8f, -2.0f}};
    const AABB far{{4.4f, 64.0f, -0.3f}, {5.0f, 65.8f, 0.3f}};
    const AABB tall{{2.0f, 70.0f, -0.3f}, {2.6f, 72.0f, 0.3f}};
    assert(validBox(player) && !validBox(AABB{{NAN, 0, 0}, {1, 1, 1}}));
    assert(std::abs(feetCenter(player).y - (64.0f + groundLift)) < 0.0001f);
    assert(std::abs(eyePosition(player).y - (64.0f + 1.62f)) < 0.0001f);
    const Vec3 eye = eyePosition(player);
    const Vec3 forward = lookVector(Vec2{0, 0});
    assert(std::abs(forward.x) < 0.0001f && std::abs(forward.y) < 0.0001f);
    assert(std::abs(forward.z - 1.0f) < 0.0001f);
    const Vec3 down = lookVector(Vec2{90, 0});
    assert(std::abs(down.y + 1.0f) < 0.0001f);
    assert(std::abs(std::hypot(down.x, down.z)) < 0.0001f);

    assert(boxesWithin(player, nearSide, 3.0f));
    assert(!boxesWithin(player, far, 3.0f));
    assert(boxesWithin(player, far, 5.0f));
    assert(!boxesWithin(player, tall, 3.0f));
    assert(boxDistanceSq(player, player) == 0.0f);

    // AABB proximity is no longer enough: only the eye-ray hit counts.
    assert(rayHitsAABB(eye, forward, nearFront, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearBehind, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearSide, 3.0f)); // beside the player, not aimed at
    assert(!rayHitsAABB(eye, forward, far, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearFront, 1.0f));
    assert(!rayHitsAABB(eye, Vec3{0, 0, 0}, nearFront, 3.0f));
    assert(!rayHitsAABB(eye, forward, tall, 3.0f));

    const PlayerView view{eye, forward};
    const int dummy = 0;
    const void* picked = &dummy;

    // Authoritative game verdict: never overridden by nearby geometry.
    auto entity = classify({nearSide}, 3.0f,
        GameVerdict{true, GameHit::Entity, picked}, view);
    assert(entity.targetPresent && entity.inRange && entity.hittableActor == picked);
    auto outOfRange = classify({nearFront}, 3.0f,
        GameVerdict{true, GameHit::EntityOutOfRange, picked}, view);
    assert(outOfRange.targetPresent && !outOfRange.inRange && outOfRange.hittableActor == nullptr);
    auto other = classify({nearFront}, 3.0f,
        GameVerdict{true, GameHit::Other, nullptr}, view);
    assert(!other.targetPresent && !other.inRange);
    auto unknown = classify({nearFront}, 3.0f, GameVerdict{}, view);
    assert(unknown.targetPresent && unknown.inRange); // eye-ray fallback

    // Fallback only: an entity behind/to the side does not turn the ring green.
    auto fallback = classify({nearBehind, nearSide}, 3.0f, GameVerdict{}, view);
    assert(fallback.targetPresent && !fallback.inRange);
    auto fallbackFront = classify({nearBehind, nearFront}, 3.0f, GameVerdict{}, view);
    assert(fallbackFront.targetPresent && fallbackFront.inRange);
    auto empty = classify({}, 3.0f, GameVerdict{}, view);
    assert(!empty.targetPresent && !empty.inRange);

    hitbox::Track track{};
    assert(hitbox::pushSample(track, Vec3{0, 64, 0}, 0.05f));
    assert(hitbox::pushSample(track, Vec3{1, 64, 0}, 0.05f));
    assert(std::abs(interpolatedCenter(track, 0.0f).x - 0.0f) < 0.0001f);
    assert(std::abs(interpolatedCenter(track, 0.025f).x - 0.5f) < 0.0001f);
    assert(std::abs(interpolatedCenter(track, 0.05f).x - 1.0f) < 0.0001f);
    assert(std::abs(interpolatedCenter(track, 1.0f).x - 1.0f) < 0.0001f);

    for (int style = 0; style < jumpcircle::styleCount; ++style) {
        int count = 0;
        bool bright = false, transparent = false;
        emitRing(Vec3{2, 64, 3}, 3.0f, 0.10f, static_cast<jumpcircle::Style>(style),
                 [&](const Vec3& p, float alpha) {
                     assert(std::isfinite(p.x) && p.y == 64 && std::isfinite(p.z));
                     assert(alpha >= 0 && alpha <= 1);
                     const float radius = std::hypot(p.x - 2, p.z - 3);
                     if (style == static_cast<int>(jumpcircle::Style::Circle))
                         assert(radius >= 2.94f && radius <= 3.06f);
                     bright |= alpha > 0.9f;
                     transparent |= alpha == 0;
                     ++count;
                 });
        assert(count == vertexCount && bright && transparent);
        const auto a = jumpcircle::contour(static_cast<jumpcircle::Style>(style), 0);
        const auto b = jumpcircle::contour(static_cast<jumpcircle::Style>(style), segments);
        assert(std::abs(a.x - b.x) < 0.00001f && std::abs(a.z - b.z) < 0.00001f);
    }

    std::puts("hitrange geometry, game verdict and reach: OK");
}
