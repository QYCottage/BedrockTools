#include "modules/visual/blockoutline_geometry.hpp"

#include <cassert>
#include <iostream>

using bedrocktools::modules::blockoutline::Point;
using bedrocktools::modules::blockoutline::makeBox;

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

    std::cout << "block outline geometry tests passed\n";
    return 0;
}
