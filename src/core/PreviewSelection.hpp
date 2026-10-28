#pragma once

#include <cstdint>

namespace bedrocktools::core::preview {

struct Selection {
    void* stack = nullptr;
    void* player = nullptr;
    void* level = nullptr;
    void* dimension = nullptr;
    std::uint64_t revision = 0;
    bool inContainer = false;
};

bool initialize();
Selection current(void* uiContext);

}
