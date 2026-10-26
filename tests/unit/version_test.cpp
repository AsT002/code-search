#include "codesearch/version.hpp"

#include <gtest/gtest.h>

TEST(Version, MatchesProjectVersion) {
    EXPECT_EQ(codesearch::version(), EXPECTED_VERSION);
}
