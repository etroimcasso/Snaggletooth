#include "snaggletooth/version.h"

#include <gtest/gtest.h>

#include <string_view>

TEST(Version, ReturnsANonEmptyDottedTriple) {
    const std::string_view v = snaggletooth::version();
    EXPECT_FALSE(v.empty());
    EXPECT_NE(v.find('.'), std::string_view::npos);
}
