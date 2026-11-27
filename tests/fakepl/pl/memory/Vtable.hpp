#pragma once

#include <cstddef>
#include <cstdint>

namespace pl::memory {

inline std::uintptr_t resolveVtableFunction(const char*, std::size_t, const char*) {
    return 0;
}

}
