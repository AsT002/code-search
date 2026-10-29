#include "codesearch/util/object_id.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace codesearch {
namespace {

constexpr std::string_view kSampleHex = "0123456789abcdef0123456789abcdef01234567";

TEST(ObjectId, AcceptsFullLowercaseHex) {
    const std::optional<ObjectId> id = ObjectId::fromHex(kSampleHex);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->hex(), kSampleHex);
}

TEST(ObjectId, RejectsAnythingElse) {
    EXPECT_FALSE(ObjectId::fromHex("").has_value());
    EXPECT_FALSE(ObjectId::fromHex(kSampleHex.substr(1)).has_value());
    EXPECT_FALSE(ObjectId::fromHex(std::string(kSampleHex) + "0").has_value());
    EXPECT_FALSE(ObjectId::fromHex("0123456789ABCDEF0123456789abcdef01234567").has_value());
    EXPECT_FALSE(ObjectId::fromHex("g123456789abcdef0123456789abcdef01234567").has_value());
}

}  // namespace
}  // namespace codesearch
