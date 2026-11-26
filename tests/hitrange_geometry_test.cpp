#include "modules/visual/hitrange_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
bool alwaysClear(const void*, int, int, int) { return false; }
bool alwaysSolid(const void*, int, int, int) { return true; }
// A one-block-high wall across z=1, at the eye's Y level. Both the centre and
// head sight lines from the test eye pass through it.
bool frontWall(const void*, int, int y, int z) { return z == 1 && y == 65; }
// Blocks the centre sight line at y=64 but leaves the head line at y=65 clear.
// Problem 5 requires one complete visible face, so this partial view stays red.
bool lowWall(const void*, int, int y, int z) { return z == 1 && y == 64; }
// Problem 6 vertical cases: a cage/support block blocks the old centre/head
// rays, while a full top/bottom face remains visible from above/below.
bool aboveSolid(const void*, int, int y, int z) { return z == 2 && y == 65; }
bool belowSolid(const void*, int, int y, int z) { return z == 1 && y == 66; }
}

int main() {
    using namespace hitrange;
    using bedrocktools::sdk::AABB;
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
    const AABB highPlayer{{-0.3f, 65.0f, -0.3f}, {0.3f, 66.8f, 0.3f}};
    const AABB lowPlayer{{-0.3f, 63.0f, -0.3f}, {0.3f, 64.8f, 0.3f}};
    const AABB elevated{{-0.3f, 65.0f, 2.0f}, {0.3f, 66.8f, 2.6f}};
    assert(validBox(player) && !validBox(AABB{{NAN, 0, 0}, {1, 1, 1}}));
    assert(std::abs(feetCenter(player).y - (64.0f + groundLift)) < 0.0001f);
    assert(std::abs(eyePosition(player).y - (64.0f + 1.62f)) < 0.0001f);
    const Vec3 eye = eyePosition(player);

    assert(boxesWithin(player, nearSide, 3.0f));
    assert(!boxesWithin(player, far, 3.0f));
    assert(boxesWithin(player, far, 5.0f));
    assert(!boxesWithin(player, tall, 3.0f));
    assert(boxDistanceSq(player, player) == 0.0f);
    assert(std::abs(pointBoxDistanceSq(eye, nearFront) - 4.0f) < 0.0001f);
    assert(pointBoxDistanceSq(eye, far) > 9.0f);
    assert(std::isinf(pointBoxDistanceSq(eye, AABB{{NAN, 0, 0}, {1, 1, 1}})));

    const SightTest clear{nullptr, &alwaysClear};
    const SightTest solidWall{nullptr, &alwaysSolid};
    const SightTest frontWallSight{nullptr, &frontWall};
    const SightTest lowWallSight{nullptr, &lowWall};
    assert(hasClearLineOfSight(eye, nearFront, clear));
    assert(!hasClearLineOfSight(eye, nearFront, frontWallSight));
    assert(!hasClearLineOfSight(eye, nearFront, lowWallSight));
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
    // Problem 5: green is a camera-independent range/LOS verdict. Every nearby
    // unobstructed target qualifies, whether it is in front, beside, behind or
    // off the crosshair. No camera direction or HitResult is an input.
    auto noCrosshair = classify(mixed, 3.0f, eye, clear);
    assert(noCrosshair.targetPresent && noCrosshair.inRange);
    assert(noCrosshair.hittableActors.size() == 4);
    assert(noCrosshair.hittableActors[0] == &frontActor);
    assert(noCrosshair.hittableActors[1] == &sideActor);
    assert(noCrosshair.hittableActors[2] == &behindActor);
    assert(noCrosshair.hittableActors[3] == &offActor);
    assert(noCrosshair.hittableActor == &frontActor);

    // Simulated 180-degree screen rotation: classify() has no look parameter,
    // so the eye/range/LOS result is exactly unchanged.
    auto turned = classify(mixed, 3.0f, eye, clear);
    assert(turned.inRange && turned.hittableActors.size() == 4);
    assert(turned.hittableActors == noCrosshair.hittableActors);

    // A wall between eye and target rejects every candidate; this is not plain
    // radial AABB proximity and cannot produce a behind-wall false green.
    auto occluded = classify(mixed, 3.0f, eye, solidWall);
    assert(occluded.targetPresent && !occluded.inRange &&
           occluded.hittableActors.empty());
    auto partialView = classify({{&frontActor, nearFront}}, 3.0f, eye, lowWallSight);
    assert(partialView.targetPresent && !partialView.inRange);

    // Distance is measured from the eye to the target box, not from boxes.
    auto tooFar = classify({{&sideActor, far}}, 3.0f, eye, clear);
    assert(tooFar.targetPresent && !tooFar.inRange);
    auto creativeFar = classify({{&sideActor, far}}, 5.0f, eye, clear);
    assert(creativeFar.targetPresent && creativeFar.inRange);

    // Problem 6: one-block vertical offsets stay green when the 3D distance is
    // inside reach and a complete top/bottom face is visible. A full wall still
    // vetoes the same vertical setup.
    const SightTest aboveSight{nullptr, &aboveSolid};
    const SightTest belowSight{nullptr, &belowSolid};
    const Vec3 highEye = eyePosition(highPlayer);
    const Vec3 lowEye = eyePosition(lowPlayer);
    assert(visibleFaceCandidates(highEye, nearFront).size() >= 2);
    assert(visibleFaceCandidates(lowEye, elevated).size() >= 2);
    auto above = classify({{&frontActor, nearFront}}, 3.0f, highEye, aboveSight);
    assert(above.targetPresent && above.inRange && above.hittableActor == &frontActor);
    auto below = classify({{&frontActor, elevated}}, 3.0f, lowEye, belowSight);
    assert(below.targetPresent && below.inRange && below.hittableActor == &frontActor);
    auto aboveWall = classify({{&frontActor, nearFront}}, 3.0f, highEye, solidWall);
    assert(aboveWall.targetPresent && !aboveWall.inRange);
    auto belowWall = classify({{&frontActor, elevated}}, 3.0f, lowEye, solidWall);
    assert(belowWall.targetPresent && !belowWall.inRange);

    // No solid-block signature: fail closed to red. There is no TypeEntity or
    // exact-crosshair fallback that can promise a hit through an unseen wall.
    auto noSignature = classify({{&frontActor, nearFront}}, 3.0f, eye);
    assert(noSignature.targetPresent && !noSignature.inRange &&
           noSignature.hittableActor == nullptr);
    auto empty = classify({}, 3.0f, eye, clear);
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

    std::puts("hitrange camera-independent 3D range, vertical LOS, stability and ring geometry: OK");
}
