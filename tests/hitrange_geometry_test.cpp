#include "modules/visual/hitrange_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
bool alwaysClear(const void*, int, int, int) { return false; }
// A one-block-high wall across z=1, at the eye's Y level. Both the centre and
// head sight lines from the test eye pass through it.
bool frontWall(const void*, int, int y, int z) { return z == 1 && y == 65; }
}

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
    const AABB offCrosshair{{1.5f, 64.0f, 1.5f}, {2.1f, 65.8f, 2.1f}};
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
    assert(std::abs(pointBoxDistanceSq(eye, nearFront) - 4.0f) < 0.0001f);
    assert(pointBoxDistanceSq(eye, far) > 9.0f);
    assert(std::isinf(pointBoxDistanceSq(eye, AABB{{NAN, 0, 0}, {1, 1, 1}})));

    // The exact-crosshair ray remains available for the unsupported-build
    // fallback, but it is no longer the only way to qualify.
    assert(rayHitsAABB(eye, forward, nearFront, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearBehind, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearSide, 3.0f));
    assert(!rayHitsAABB(eye, forward, far, 3.0f));
    assert(!rayHitsAABB(eye, forward, nearFront, 1.0f));
    assert(!rayHitsAABB(eye, Vec3{0, 0, 0}, nearFront, 3.0f));
    assert(!rayHitsAABB(eye, forward, tall, 3.0f));

    // Frontal arc: in front and off the crosshair qualify, beside/behind do not.
    assert(inFrontCone(eye, forward, nearFront));
    assert(inFrontCone(eye, forward, offCrosshair));
    assert(!inFrontCone(eye, forward, nearSide));
    assert(!inFrontCone(eye, forward, nearBehind));
    assert(!inFrontCone(eye, Vec3{0, 0, 0}, nearFront));

    const PlayerView view{eye, forward};
    const SightTest clear{nullptr, &alwaysClear};
    const SightTest wall{nullptr, &frontWall};
    assert(hasClearLineOfSight(eye, nearFront, clear));
    assert(!hasClearLineOfSight(eye, nearFront, wall));
    assert(!hasClearLineOfSight(eye, nearFront, SightTest{}));
    assert(!sightRayHitsSolid(eye, Vec3{0, 65.62f, 0}, clear));

    const int frontActor = 1;
    const int sideActor = 2;
    const int behindActor = 3;
    const int offActor = 4;
    const std::vector<TargetCandidate> mixed{
        {&frontActor, nearFront}, {&sideActor, nearSide},
        {&behindActor, nearBehind}, {&offActor, offCrosshair},
    };
    // No exact crosshair needed: TypeNoHit plus clear sight still greens both
    // front and off-axis targets, while side/behind targets stay red.
    auto noCrosshair = classify(mixed, 3.0f,
        GameVerdict{true, GameHit::Other, nullptr}, view, clear);
    assert(noCrosshair.targetPresent && noCrosshair.inRange);
    assert(noCrosshair.hittableActors.size() == 2);
    assert(noCrosshair.hittableActors[0] == &frontActor);
    assert(noCrosshair.hittableActors[1] == &offActor);
    assert(noCrosshair.hittableActor == &frontActor);

    // A wall between eye and target rejects every candidate; this is not plain
    // radial AABB proximity and cannot produce a behind-wall false green.
    auto occluded = classify(mixed, 3.0f, GameVerdict{}, view, wall);
    assert(occluded.targetPresent && !occluded.inRange &&
           occluded.hittableActors.empty());

    // The game's explicit out-of-range entity verdict remains a veto.
    auto outOfRange = classify({{&frontActor, nearFront}}, 3.0f,
        GameVerdict{true, GameHit::EntityOutOfRange, &frontActor}, view, clear);
    assert(outOfRange.targetPresent && !outOfRange.inRange);

    // Distance is measured from the eye to the target box, not from boxes.
    auto tooFar = classify({{&sideActor, far}}, 3.0f, GameVerdict{}, view, clear);
    assert(tooFar.targetPresent && !tooFar.inRange);
    auto creativeFar = classify({{&sideActor, far}}, 5.0f, GameVerdict{}, view, clear);
    assert(creativeFar.targetPresent && !creativeFar.inRange); // still beside/behind

    // With no solid-block signature, fail closed to the exact game TypeEntity.
    auto fallbackEntity = classify({{&frontActor, nearFront}}, 3.0f,
        GameVerdict{true, GameHit::Entity, &frontActor}, view);
    assert(fallbackEntity.inRange && fallbackEntity.hittableActor == &frontActor);
    auto fallbackOther = classify({{&frontActor, nearFront}}, 3.0f,
        GameVerdict{true, GameHit::Other, nullptr}, view);
    assert(fallbackOther.targetPresent && !fallbackOther.inRange);
    auto empty = classify({}, 3.0f, GameVerdict{}, view, clear);
    assert(!empty.targetPresent && !empty.inRange && empty.hittableActor == nullptr);

    VerdictHold hold;
    assert(!hold.update(false, 0.00f));
    assert(hold.update(true, 0.03f));                 // green enters immediately
    assert(hold.update(true, 0.08f));
    assert(hold.update(false, 0.10f));                // brief miss holds green
    assert(!hold.update(false, 0.30));                // then red
    assert(hold.update(true, 0.31));
    assert(hold.update(false, -1.0f) == true);        // clock skew cannot flash

    StableMode mode;
    assert(!mode.observe(true));                     // one weak hint is ignored
    assert(mode.observe(true));                      // two consistent hints win
    assert(mode.observe(false) == true);
    assert(!mode.observe(false));
    mode.reset(false);
    assert(!mode.creative && mode.pending == -1 && mode.streak == 0);

    const Vec3 center0{0, 64, 0};
    const Vec3 center1{10, 64, 0};
    const auto stepped = stepToward(center0, center1, maxCenterStep);
    assert(std::abs(stepped.x - maxCenterStep) < 0.0001f);
    assert(std::abs(stepped.y - 64.0f) < 0.0001f);
    const auto unchanged = stepToward(center0, Vec3{NAN, 64, 0}, maxCenterStep);
    assert(unchanged.x == 0 && unchanged.y == 64);
    const auto nearby = stepToward(center0, Vec3{0.1f, 64, 0}, maxCenterStep);
    assert(nearby.x == 0.1f);

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

    std::puts("hitrange range, frontal LOS, stability and ring geometry: OK");
}
