#include "snaggletooth/version.h"

namespace snaggletooth {

namespace {
constexpr std::string_view kVersion = "0.0.1";
}

std::string_view version() noexcept { return kVersion; }

}  // namespace snaggletooth
