#include "modules/visual/hitrange_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    using namespace hitrange;
    using bedrocktools::sdk::AABB;
    using bedrocktools::sdk::Vec3;

    assert(safeReach(3.0f) == 3.0f);
    assert(safeReach(-10.0f) == minReach);
    assert(safeReach(100.0f) == maxReach);
    assert(std::isnan(safeReach(NAN)) == false); // falls back to 3, never NaN
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
    const AABB near{{2.4f, 64.0f, -0.3f}, {3.0f, 65.8f, 0.3f}};
    const AABB far{{4.4f, 64.0f, -0.3f}, {5.0f, 65.8f, 0.3f}};
    const AABB tall{{2.0f, 70.0f, -0.3f}, {2.6f, 72.0f, 0.3f}};
    assert(validBox(player) && !validBox(AABB{{NAN, 0, 0}, {1, 1, 1}}));
    assert(std::abs(feetCenter(player).y - (64.0f + groundLift)) < 0.0001f);
    assert(inReach(player, near, 3.0f));
    assert(!inReach(player, far, 3.0f));
    assert(inReach(player, far, 5.0f));
    assert(!inReach(player, tall, 3.0f)); // vertical separation still counts
    assert(boxDistanceSq(player, player) == 0.0f);
    assert(boxDistanceSq(player, near) > 0.0f);

    assert(classify(player, {}, 3.0f).targetPresent == false);
    assert(classify(player, {}, 3.0f).inRange == false);
    assert(classify(player, {near}, 3.0f).inRange);
    assert(classify(player, {near}, 3.0f).targetPresent);
    assert(!classify(player, {far}, 3.0f).inRange);
    assert(classify(player, {far}, 5.0f).inRange);
    assert(classify(player, {}, 3.0f, true).inRange); // game crosshair verdict
    assert(classify(player, {}, 3.0f, true).targetPresent);
    assert(classify(player, {}, 3.0f, false, true).targetPresent);
    assert(!classify(player, {}, 3.0f, false, true).inRange);

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

    std::puts("hitrange geometry and reach: OK");
}
