#include "snaggletooth/version.h"

namespace snaggletooth {

namespace {
constexpr std::string_view kVersion = "0.9.0";
}

std::string_view version() noexcept { return kVersion; }

}  // namespace snaggletooth
