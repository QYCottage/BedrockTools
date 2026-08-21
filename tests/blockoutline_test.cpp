#include "modules/visual/blockoutline_geometry.hpp"

#include <cassert>
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

    std::cout << "block outline geometry tests passed\n";
    return 0;
}
