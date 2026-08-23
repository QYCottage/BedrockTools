#pragma once
//
// Minimal host-side stand-in for the fmt headers that xmake provides on
// Android (pl/Logger.hpp includes <fmt/format.h> and <fmt/std.h>).
//
// The host unit tests never assert on log output, so vformat returns an empty
// string; the point is only that the small API surface used by the preloader
// headers compiles. Host tests only.

#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace fmt {

using string_view = std::string_view;

template <typename... Args>
auto make_format_args(Args&&... args) {
    return std::make_tuple(std::forward<Args>(args)...);
}

template <typename FormatArgs>
std::string vformat(string_view, const FormatArgs&) {
    return std::string();
}

} // namespace fmt
