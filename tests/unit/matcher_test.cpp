#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "codesearch/search/evaluator.hpp"
#include "codesearch/search/literal_matcher.hpp"

namespace codesearch {
namespace {

std::vector<Span> findAll(std::string_view pattern, std::string_view text, bool caseSensitive) {
    std::vector<Span> spans;
    LiteralMatcher(pattern, caseSensitive).findAll(text, [&](Span span) {
        spans.push_back(span);
        return true;
    });
    return spans;
}

TEST(LiteralMatcher, FindsEveryMatch) {
    EXPECT_EQ(findAll("ab", "ab cab", true), (std::vector<Span>{{0, 2}, {4, 6}}));
}

TEST(LiteralMatcher, MatchesDontOverlap) {
    EXPECT_EQ(findAll("aa", "aaaaa", true), (std::vector<Span>{{0, 2}, {2, 4}}));
}

TEST(LiteralMatcher, CaseSensitivity) {
    EXPECT_TRUE(findAll("Foo", "foo", true).empty());
    EXPECT_EQ(findAll("Foo", "xFOO", false), (std::vector<Span>{{1, 4}}));
}

TEST(LiteralMatcher, DoesNotFoldNonAscii) {
    EXPECT_TRUE(findAll("\xC9", "\xE9", false).empty());
    EXPECT_EQ(findAll("\xC9", "a\xC9", false), (std::vector<Span>{{1, 2}}));
}

TEST(LiteralMatcher, EmptyPatternMatchesNothing) {
    EXPECT_TRUE(findAll("", "abc", true).empty());
}

TEST(LiteralMatcher, StopsWhenAsked) {
    int calls = 0;
    LiteralMatcher("a", true).findAll("aaa", [&](Span) {
        ++calls;
        return false;
    });
    EXPECT_EQ(calls, 1);
}

TEST(Evaluator, AndNeedsEveryLiteral) {
    const Evaluator evaluator(allOf({literal("foo"), literal("bar")}), {});
    EXPECT_TRUE(evaluator.matches("bar then foo"));
    EXPECT_FALSE(evaluator.matches("only foo"));
}

TEST(Evaluator, EmptyLiteralMatchesWithoutSpans) {
    const Evaluator evaluator(literal(""), {});
    EXPECT_TRUE(evaluator.matches("anything"));
    EXPECT_TRUE(evaluator.collectSpans("anything").empty());
}

TEST(Evaluator, SpansAreSortedWithoutDuplicates) {
    const Evaluator evaluator(allOf({literal("b"), literal("a"), literal("a")}), {});
    EXPECT_EQ(evaluator.collectSpans("ab a"), (std::vector<Span>{{0, 1}, {1, 2}, {3, 4}}));
}

}  // namespace
}  // namespace codesearch
