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

TEST(ReplaceInvalidUtf8, ReplacesEachBadByte) {
    EXPECT_EQ(replaceInvalidUtf8("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(replaceInvalidUtf8("a\xFF"
                                 "b"),
              "a\xEF\xBF\xBD"
              "b");
    // A cut-off character: each of its bytes is replaced.
    EXPECT_EQ(replaceInvalidUtf8("\xE2\x82"), "\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT_TRUE(isValidUtf8(replaceInvalidUtf8("\xC0\x80\xED\xA0\x80\xF5")));
}

TEST(Utf8, EscapeForTerminal) {
    EXPECT_EQ(escapeForTerminal("plain text\twith tab"), "plain text\twith tab");
    EXPECT_EQ(escapeForTerminal("\x1b[31mred\x07"), "\\x1b[31mred\\x07");
    EXPECT_EQ(escapeForTerminal(std::string("a\0b\x7f", 4)), "a\\x00b\\x7f");
    EXPECT_EQ(escapeForTerminal("\r\n"), "\\x0d\\x0a");
    // C1 controls as UTF-8, and a lone byte that some terminals read as CSI.
    // ("\x9b" "2J": a hex escape would also take the 2.)
    EXPECT_EQ(escapeForTerminal("x\xc2\x9b"
                                "2Jy"),
              "x\\xc2\\x9b2Jy");
    EXPECT_EQ(escapeForTerminal("x\x9b"
                                "2J"),
              "x\\x9b2J");
    // Other characters stay, even ones with 0x80 to 0x9F inside them.
    EXPECT_EQ(escapeForTerminal("caf\xc3\xa9 \xe2\x80\xba \xc2\xa0"),
              "caf\xc3\xa9 \xe2\x80\xba \xc2\xa0");
    EXPECT_EQ(escapeForTerminal("\xff"), "\\xff");
}

}  // namespace
}  // namespace codesearch
