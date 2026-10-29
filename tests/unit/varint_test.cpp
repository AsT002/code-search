#include "codesearch/util/varint.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace codesearch {
namespace {

TEST(Varint, RoundTrips) {
    const std::vector<std::uint32_t> values = {
        0,     1,     127,       128,       300,
        16383, 16384, 1U << 21U, 1U << 28U, std::numeric_limits<std::uint32_t>::max(),
    };
    std::string bytes;
    for (const std::uint32_t value : values) {
        appendVarint(bytes, value);
    }
    std::size_t pos = 0;
    for (const std::uint32_t value : values) {
        EXPECT_EQ(readVarint(bytes, pos), value);
    }
    EXPECT_EQ(pos, bytes.size());
}

TEST(Varint, UsesFewBytesForSmallNumbers) {
    const auto size = [](std::uint32_t value) {
        std::string bytes;
        appendVarint(bytes, value);
        return bytes.size();
    };
    EXPECT_EQ(size(0), 1U);
    EXPECT_EQ(size(127), 1U);
    EXPECT_EQ(size(128), 2U);
    EXPECT_EQ(size(std::numeric_limits<std::uint32_t>::max()), 5U);

    std::string bytes;
    appendVarint(bytes, 300);
    EXPECT_EQ(bytes, "\xAC\x02");  // 300 = 0b10'0101100: low 7 bits first
}

TEST(Varint, RejectsBrokenInput) {
    std::size_t pos = 0;
    EXPECT_EQ(readVarint("", pos), std::nullopt);
    EXPECT_EQ(readVarint("\x80", pos), std::nullopt);  // says more follows, but ends
    EXPECT_EQ(readVarint("\xFF\xFF\xFF\xFF\x10", pos), std::nullopt);      // above 32 bits
    EXPECT_EQ(readVarint("\xFF\xFF\xFF\xFF\x8F\x01", pos), std::nullopt);  // over 5 bytes
    EXPECT_EQ(pos, 0U);  // failures leave pos alone

    EXPECT_EQ(readVarint("\xFF\xFF\xFF\xFF\x0F", pos), std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(pos, 5U);
}

TEST(Varint, ReadsFromTheMiddle) {
    const std::string bytes = "\x05\xAC\x02\x07";
    std::size_t pos = 1;
    EXPECT_EQ(readVarint(bytes, pos), 300U);
    EXPECT_EQ(pos, 3U);
    EXPECT_EQ(readVarint(bytes, pos), 7U);
    EXPECT_EQ(readVarint(bytes, pos), std::nullopt);  // at the end
}

}  // namespace
}  // namespace codesearch
