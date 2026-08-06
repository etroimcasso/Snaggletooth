#pragma once

// Library identification.

#include <string_view>

namespace snaggletooth {

// The library's version as a "major.minor.patch" string — the value this build was produced
// from. The view refers to static storage and is valid for the life of the process.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace snaggletooth
