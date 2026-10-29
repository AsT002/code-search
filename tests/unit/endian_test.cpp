#include "codesearch/util/endian.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace codesearch {
namespace {

TEST(Endian, WritesLowestByteFirst) {
    std::string bytes;
    appendU32(bytes, 0x11223344);
    EXPECT_EQ(bytes, "\x44\x33\x22\x11");
    appendU16(bytes, 0xABCD);
    EXPECT_EQ(bytes.substr(4), "\xCD\xAB");
}

TEST(Endian, RoundTrips) {
    std::string bytes;
    appendU8(bytes, 0xFE);
    appendU16(bytes, 0xBEEF);
    appendU32(bytes, 0xDEADBEEF);
    appendU64(bytes, 0x0123456789ABCDEFULL);
    EXPECT_EQ(bytes.size(), 15U);
    EXPECT_EQ(readU8(bytes, 0), 0xFE);
    EXPECT_EQ(readU16(bytes, 1), 0xBEEF);
    EXPECT_EQ(readU32(bytes, 3), 0xDEADBEEFU);
    EXPECT_EQ(readU64(bytes, 7), 0x0123456789ABCDEFULL);
}

TEST(Endian, RefusesToReadPastTheEnd) {
    const std::string bytes(8, '\0');
    EXPECT_EQ(readU64(bytes, 0), 0U);
    EXPECT_THROW(readU64(bytes, 1), std::out_of_range);
    EXPECT_THROW(readU32(bytes, 5), std::out_of_range);
    EXPECT_THROW(readU8(bytes, 8), std::out_of_range);
    EXPECT_THROW(readU32(bytes, static_cast<std::size_t>(-1)), std::out_of_range);  // no overflow
}

}  // namespace
}  // namespace codesearch
