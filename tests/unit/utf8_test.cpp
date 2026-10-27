#include "codesearch/util/utf8.hpp"

#include <gtest/gtest.h>

#include <string>

namespace codesearch {
namespace {

TEST(IsValidUtf8, AcceptsWellFormedText) {
    EXPECT_TRUE(isValidUtf8(""));
    EXPECT_TRUE(isValidUtf8("plain ascii\n"));
    EXPECT_TRUE(isValidUtf8(std::string("nul \0 is ASCII", 14)));
    EXPECT_TRUE(isValidUtf8("caf\xC3\xA9"));       // é, 2 bytes
    EXPECT_TRUE(isValidUtf8("\xE2\x82\xAC"));      // €, 3 bytes
    EXPECT_TRUE(isValidUtf8("\xF0\x9F\x98\x80"));  // 😀, 4 bytes
    EXPECT_TRUE(isValidUtf8("\xED\x9F\xBF"));      // U+D7FF, just below surrogates
    EXPECT_TRUE(isValidUtf8("\xEE\x80\x80"));      // U+E000, just above surrogates
    EXPECT_TRUE(isValidUtf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF, the highest
}

TEST(IsValidUtf8, RejectsBadBytes) {
    EXPECT_FALSE(isValidUtf8("\xFF"));
    EXPECT_FALSE(isValidUtf8("\xF5\x80\x80\x80"));  // F5 and up are never used
    EXPECT_FALSE(isValidUtf8("\x80"));              // continuation byte on its own
    EXPECT_FALSE(isValidUtf8("a\xC3"));             // ends mid-character
    EXPECT_FALSE(isValidUtf8("\xE2\x82"));          // ends mid-character
    EXPECT_FALSE(isValidUtf8("\xC3\x28"));          // second byte isn't a continuation
}

TEST(IsValidUtf8, RejectsOverlongSurrogatesAndTooHigh) {
    EXPECT_FALSE(isValidUtf8("\xC0\x80"));          // overlong NUL
    EXPECT_FALSE(isValidUtf8("\xC1\xBF"));          // overlong
    EXPECT_FALSE(isValidUtf8("\xE0\x80\x80"));      // overlong
    EXPECT_FALSE(isValidUtf8("\xF0\x80\x80\x80"));  // overlong
    EXPECT_FALSE(isValidUtf8("\xED\xA0\x80"));      // U+D800, a surrogate
    EXPECT_FALSE(isValidUtf8("\xF4\x90\x80\x80"));  // U+110000, too high
}

}  // namespace
}  // namespace codesearch
