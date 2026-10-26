#include "codesearch/index/trigram.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "codesearch/util/ascii.hpp"

namespace codesearch {
namespace {

TEST(FoldAscii, LowercasesOnlyAsciiLetters) {
    EXPECT_EQ(foldAscii('A'), 'a');
    EXPECT_EQ(foldAscii('Z'), 'z');
    EXPECT_EQ(foldAscii('a'), 'a');
    EXPECT_EQ(foldAscii('0'), '0');
    EXPECT_EQ(foldAscii('['), '[');
    EXPECT_EQ(foldAscii('\xC9'), '\xC9');  // 'É' in Latin-1 stays as is
}

TEST(PackTrigram, PutsBytesInOrder) {
    EXPECT_EQ(packTrigram('a', 'b', 'c'), 0x616263U);
    EXPECT_EQ(packTrigram('\xFF', '\0', '\x01'), 0xFF0001U);
}

TEST(ExtractTrigrams, ShortTextHasNone) {
    EXPECT_TRUE(extractTrigrams("").empty());
    EXPECT_TRUE(extractTrigrams("ab").empty());
}

TEST(ExtractTrigrams, ReturnsEachWindowSorted) {
    const std::vector<Trigram> expected = {packTrigram('a', 'b', 'c'), packTrigram('b', 'c', 'd')};
    EXPECT_EQ(extractTrigrams("abcd"), expected);
    EXPECT_EQ(extractTrigrams("dcb"), std::vector<Trigram>{packTrigram('d', 'c', 'b')});
}

TEST(ExtractTrigrams, RemovesDuplicates) {
    EXPECT_EQ(extractTrigrams("aaaaaa"), std::vector<Trigram>{packTrigram('a', 'a', 'a')});
}

TEST(ExtractTrigrams, FoldsCase) {
    EXPECT_EQ(extractTrigrams("AbC"), extractTrigrams("abc"));
}

TEST(ExtractTrigrams, KeepsNonAsciiBytes) {
    EXPECT_EQ(extractTrigrams("a\xC9z"), std::vector<Trigram>{packTrigram('a', '\xC9', 'z')});
}

}  // namespace
}  // namespace codesearch
