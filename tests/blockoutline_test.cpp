#include "modules/visual/blockoutline_color.hpp"
#include "modules/visual/blockoutline_geometry.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using bedrocktools::modules::blockoutline::Point;
using bedrocktools::modules::blockoutline::makeBox;
using bedrocktools::modules::blockoutline::makeFaces;

int main() {
    constexpr auto box = makeBox(10.0f, -2.0f, 4.0f, 0.0f);
    static_assert(box.size() == 12);

    // Every cube corner must be touched by exactly three edges.
    constexpr Point corners[] = {
        {10, -2, 4}, {11, -2, 4}, {10, -1, 4}, {11, -1, 4},
        {10, -2, 5}, {11, -2, 5}, {10, -1, 5}, {11, -1, 5},
    };
    for (const auto& corner : corners) {
        int touches = 0;
        for (const auto& line : box) {
            if (line.from == corner) ++touches;
            if (line.to == corner) ++touches;
        }
        assert(touches == 3);
    }

    // The small expansion keeps the overlay from fighting the block mesh.
    constexpr auto expanded = makeBox(0, 0, 0);
    static_assert(expanded[0].from.x < 0.0f);
    static_assert(expanded[5].to.z > 1.0f);

    // The 3D fill mode emits exactly six faces.
    constexpr auto faces = makeFaces(10.0f, -2.0f, 4.0f, 0.0f);
    static_assert(faces.size() == 6);

    // Every cube corner must be touched by exactly three faces.
    for (const auto& corner : corners) {
        int touches = 0;
        for (const auto& face : faces) {
            const Point quad[4] = {face.a, face.b, face.c, face.d};
            for (const auto& v : quad) {
                if (v == corner) ++touches;
            }
        }
        assert(touches == 3);
    }

    // Each face is a unit square (no degenerate quads).
    for (const auto& face : faces) {
        const float dx01 = face.b.x - face.a.x;
        const float dy01 = face.b.y - face.a.y;
        const float dz01 = face.b.z - face.a.z;
        const float dx03 = face.d.x - face.a.x;
        const float dy03 = face.d.y - face.a.y;
        const float dz03 = face.d.z - face.a.z;
        // Two adjacent edges of a face are perpendicular.
        assert(dx01 * dx03 + dy01 * dy03 + dz01 * dz03 == 0.0f);
        // And both are unit length.
        const float l01 = dx01 * dx01 + dy01 * dy01 + dz01 * dz01;
        const float l03 = dx03 * dx03 + dy03 * dy03 + dz03 * dz03;
        assert(l01 == 1.0f);
        assert(l03 == 1.0f);
    }

    // --- Face visibility -------------------------------------------------
    using bedrocktools::modules::blockoutline::makeFaceVisibility;

    // From straight above only the +Y face (index 1) faces the eye.
    {
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.001f);
        constexpr auto vis = makeFaceVisibility(faces, Point{0.5f, 2.0f, 0.5f});
        static_assert(!vis[0] && vis[1] && !vis[2] && !vis[3] && !vis[4] && !vis[5]);
    }
    // From a corner three faces face the eye.
    {
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.001f);
        constexpr auto vis = makeFaceVisibility(faces, Point{2.0f, 3.0f, 4.0f});
        static_assert(vis[1] && vis[3] && vis[5]);  // +Y, +Z, +X.
        static_assert(!vis[0] && !vis[2] && !vis[4]);
    }
    // Inside the box every face is kept (the outline surrounds the player).
    {
        constexpr auto faces = makeFaces(10.0f, -2.0f, 4.0f, 0.0f);
        constexpr auto vis = makeFaceVisibility(faces, Point{10.5f, -1.5f, 4.5f});
        static_assert(vis[0] && vis[1] && vis[2] && vis[3] && vis[4] && vis[5]);
    }

    // --- Edge visibility -------------------------------------------------
    using bedrocktools::modules::blockoutline::makeEdgeVisibility;

    // From straight above only the four top edges (indices 4-7) are visible.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{0.5f, 2.0f, 0.5f});
        static_assert(!vis[0] && !vis[1] && !vis[2] && !vis[3]);
        static_assert(vis[4] && vis[5] && vis[6] && vis[7]);
        static_assert(!vis[8] && !vis[9] && !vis[10] && !vis[11]);
    }
    // From straight along +X only the four edges lying in the +X plane are
    // visible: bottom edge 1, top edge 5, and the vertical edges 9 and 10.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{3.0f, 0.5f, 0.5f});
        static_assert(vis[1] && vis[5] && vis[9] && vis[10]);
        static_assert(!vis[0] && !vis[2] && !vis[3] && !vis[4]);
        static_assert(!vis[6] && !vis[7] && !vis[8] && !vis[11]);
    }
    // Every edge is visible from inside the box.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.002f);
        constexpr auto vis = makeEdgeVisibility(box, Point{0.5f, 0.5f, 0.5f});
        int visibleCount = 0;
        for (bool v : vis) visibleCount += v ? 1 : 0;
        assert(visibleCount == 12);
    }
    // The expand used by the renderer still yields sensible counts from a
    // generic diagonal viewpoint: a silhouette frame, never zero edges.
    {
        constexpr auto box = makeBox(10.0f, -2.0f, 4.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{13.0f, 0.5f, 7.0f});
        int visibleCount = 0;
        for (bool v : vis) visibleCount += v ? 1 : 0;
        assert(visibleCount >= 4 && visibleCount <= 12);
    }

    // --- Edge silhouette -------------------------------------------------
    // The silhouette is the boundary of the block's projected shape: exactly
    // the edges where one touching face faces the eye and the other does not.
    // From a corner that is the six-edge hexagon; the three edges meeting at
    // the near corner (indices 5, 6 and 10 for this eye) sit inside the
    // projection and must NOT be drawn, because as thick camera-facing quads
    // they make the outline read as a 3D box.
    using bedrocktools::modules::blockoutline::makeEdgeSilhouette;

    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeSilhouette(box, Point{2.0f, 3.0f, 4.0f});
        static_assert(vis[1] && vis[2] && vis[4] && vis[7] && vis[9] && vis[11]);
        static_assert(!vis[0] && !vis[3] && !vis[5] && !vis[6] && !vis[8] && !vis[10]);
        int count = 0;
        for (bool v : vis) count += v ? 1 : 0;
        assert(count == 6);
    }
    // From straight above only the four top edges form the outline.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeSilhouette(box, Point{0.5f, 2.0f, 0.5f});
        static_assert(!vis[0] && !vis[1] && !vis[2] && !vis[3]);
        static_assert(vis[4] && vis[5] && vis[6] && vis[7]);
        static_assert(!vis[8] && !vis[9] && !vis[10] && !vis[11]);
    }
    // From straight along +X only the four edges lying in the +X plane.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeSilhouette(box, Point{3.0f, 0.5f, 0.5f});
        static_assert(vis[1] && vis[5] && vis[9] && vis[10]);
        static_assert(!vis[0] && !vis[2] && !vis[3] && !vis[4]);
        static_assert(!vis[6] && !vis[7] && !vis[8] && !vis[11]);
    }
    // Slightly off-axis (front + a sliver of the top): still a six-edge
    // outline, never fewer than the four-edge silhouette of one face.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeSilhouette(box, Point{0.5f, 1.2f, 4.5f});
        int count = 0;
        for (bool v : vis) count += v ? 1 : 0;
        assert(count == 6);
    }
    // From inside the box every edge is kept (the outline surrounds the
    // player; culling anything would lose the frame).
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.002f);
        constexpr auto vis = makeEdgeSilhouette(box, Point{0.5f, 0.5f, 0.5f});
        int count = 0;
        for (bool v : vis) count += v ? 1 : 0;
        assert(count == 12);
    }
    // The silhouette is always a subset of the face-visible edge set.
    {
        constexpr auto box = makeBox(10.0f, -2.0f, 4.0f);
        constexpr auto full = makeEdgeVisibility(box, Point{13.0f, 0.5f, 7.0f});
        constexpr auto sil = makeEdgeSilhouette(box, Point{13.0f, 0.5f, 7.0f});
        int count = 0;
        for (std::size_t i = 0; i < full.size(); ++i) {
            assert(!(sil[i] && !full[i]));
            count += sil[i] ? 1 : 0;
        }
        assert(count == 6);
    }

    // --- RGB rainbow cycle -----------------------------------------------
    using bedrocktools::modules::blockoutline::rainbowRgb;
    using bedrocktools::modules::blockoutline::wrapPhase;

    // Exact points of the cycle (phases chosen so every value is exactly
    // representable as a float).
    constexpr auto cRed = rainbowRgb(0.0f);
    static_assert(cRed.r == 1.0f && cRed.g == 0.0f && cRed.b == 0.0f);
    constexpr auto cSpringGreen = rainbowRgb(0.25f);
    static_assert(cSpringGreen.r == 0.5f && cSpringGreen.g == 1.0f && cSpringGreen.b == 0.0f);
    constexpr auto cCyan = rainbowRgb(0.5f);
    static_assert(cCyan.r == 0.0f && cCyan.g == 1.0f && cCyan.b == 1.0f);
    constexpr auto cViolet = rainbowRgb(0.75f);
    static_assert(cViolet.r == 0.5f && cViolet.g == 0.0f && cViolet.b == 1.0f);

    // The cycle wraps: phase 1.0 is back to red, negatives wrap around.
    constexpr auto cWrapped = rainbowRgb(1.0f);
    static_assert(cWrapped.r == 1.0f && cWrapped.g == 0.0f && cWrapped.b == 0.0f);
    static_assert(wrapPhase(2.25f) == 0.25f);
    static_assert(wrapPhase(-0.25f) == 0.75f);
    static_assert(wrapPhase(1.0f) == 0.0f);

    // The primary colors sit at thirds of the cycle (checked with a small
    // epsilon because 1/3 is not exactly representable).
    const auto near = [](bedrocktools::modules::blockoutline::RgbColor c,
                         float r, float g, float b) {
        return std::fabs(c.r - r) < 1e-4f &&
               std::fabs(c.g - g) < 1e-4f &&
               std::fabs(c.b - b) < 1e-4f;
    };
    assert(near(rainbowRgb(1.0f / 3.0f), 0.0f, 1.0f, 0.0f));
    assert(near(rainbowRgb(2.0f / 3.0f), 0.0f, 0.0f, 1.0f));

    // Every channel always stays inside [0, 1].
    for (int i = 0; i < 360; ++i) {
        const auto c = rainbowRgb(static_cast<float>(i) / 360.0f);
        assert(c.r >= 0.0f && c.r <= 1.0f);
        assert(c.g >= 0.0f && c.g <= 1.0f);
        assert(c.b >= 0.0f && c.b <= 1.0f);
    }

    std::cout << "block outline geometry tests passed\n";
    return 0;
}
