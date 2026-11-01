#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "codesearch/search/evaluator.hpp"
#include "codesearch/search/literal_matcher.hpp"
#include "codesearch/search/regex_matcher.hpp"

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
    EXPECT_TRUE(evaluator.matches("f.txt", "bar then foo"));
    EXPECT_FALSE(evaluator.matches("f.txt", "only foo"));
}

TEST(Evaluator, EmptyLiteralMatchesWithoutSpans) {
    const Evaluator evaluator(literal(""), {});
    EXPECT_TRUE(evaluator.matches("f.txt", "anything"));
    EXPECT_TRUE(evaluator.collectSpans("f.txt", "anything").empty());
}

TEST(Evaluator, SpansAreSortedWithoutDuplicates) {
    const Evaluator evaluator(allOf({literal("b"), literal("a"), literal("a")}), {});
    EXPECT_EQ(evaluator.collectSpans("f.txt", "ab a"), (std::vector<Span>{{0, 1}, {1, 2}, {3, 4}}));
}

TEST(Evaluator, OrNeedsOneChild) {
    const Evaluator evaluator(anyOf({literal("foo"), literal("bar")}), {});
    EXPECT_TRUE(evaluator.matches("f.txt", "only foo"));
    EXPECT_TRUE(evaluator.matches("f.txt", "only bar"));
    EXPECT_FALSE(evaluator.matches("f.txt", "neither"));
}

TEST(Evaluator, NotExcludes) {
    const Evaluator evaluator(allOf({literal("foo"), negate(literal("bar"))}), {});
    EXPECT_TRUE(evaluator.matches("f.txt", "foo"));
    EXPECT_FALSE(evaluator.matches("f.txt", "foo bar"));
}

TEST(Evaluator, PathAndLangLookAtThePath) {
    const SearchOptions caseSensitive{.caseSensitive = true};
    EXPECT_TRUE(Evaluator(pathFilter("SRC/"), {}).matches("src/a.go", ""));
    EXPECT_FALSE(Evaluator(pathFilter("SRC/"), caseSensitive).matches("src/a.go", ""));
    EXPECT_FALSE(Evaluator(pathFilter("src/"), {}).matches("lib/a.go", "src/"));

    EXPECT_TRUE(Evaluator(langFilter("go"), {}).matches("src/a.go", ""));
    EXPECT_FALSE(Evaluator(langFilter("go"), {}).matches("src/a.c", ""));
    EXPECT_TRUE(Evaluator(constant(true), {}).matches("x", ""));
    EXPECT_FALSE(Evaluator(constant(false), {}).matches("x", ""));
}

TEST(Evaluator, HighlightsOnlyTheMatchingParts) {
    // Text under '-' is never highlighted.
    const Evaluator excluded(allOf({literal("a"), negate(literal("zzz"))}), {});
    EXPECT_EQ(excluded.collectSpans("f.txt", "a b"), (std::vector<Span>{{0, 1}}));

    // In an or, only branches that matched: "c" is in the file, but its
    // branch needs "zzz" too.
    const Evaluator either(anyOf({literal("a"), allOf({literal("c"), literal("zzz")})}), {});
    EXPECT_EQ(either.collectSpans("f.txt", "a c"), (std::vector<Span>{{0, 1}}));

    // A branch that matched through the path still highlights its text.
    const Evaluator byPath(anyOf({allOf({pathFilter(".go"), literal("c")}), literal("a")}), {});
    EXPECT_EQ(byPath.collectSpans("x.go", "a c"), (std::vector<Span>{{0, 1}, {2, 3}}));
    EXPECT_EQ(byPath.collectSpans("x.py", "a c"), (std::vector<Span>{{0, 1}}));
}

TEST(Evaluator, RepoFiltersMustBeResolvedFirst) {
    EXPECT_THROW(Evaluator(allOf({literal("a"), repoFilter("x")}), {}), std::logic_error);
}

std::vector<Span> regexSpans(std::string_view pattern, std::string_view text,
                             bool caseSensitive = true) {
    std::vector<Span> spans;
    RegexMatcher(pattern, caseSensitive).findAll(text, [&](Span span) {
        spans.push_back(span);
        return true;
    });
    return spans;
}

TEST(RegexMatcher, FindsEachMatchOnce) {
    EXPECT_EQ(regexSpans("a+", "baaab a"), (std::vector<Span>{{1, 4}, {6, 7}}));
    EXPECT_EQ(regexSpans("foo|foobar", "foobar"),
              (std::vector<Span>{{0, 3}}));  // first branch wins
    EXPECT_TRUE(regexSpans("z", "abc").empty());
}

// ^ and $ work on every line, not just the whole text. $ is before the LF,
// so a CR before it is part of the line's text.
TEST(RegexMatcher, AnchorsWorkOnEachLine) {
    EXPECT_EQ(regexSpans("^b", "ab\nbc"), (std::vector<Span>{{3, 4}}));
    EXPECT_EQ(regexSpans("b$", "ab\nbc"), (std::vector<Span>{{1, 2}}));
    EXPECT_EQ(regexSpans("b\r?$", "ab\r\n"), (std::vector<Span>{{1, 3}}));
}

// An empty match makes the file match, but there's nothing to highlight.
TEST(RegexMatcher, EmptyMatchesCountButArentShown) {
    const RegexMatcher star("x*", true);
    EXPECT_TRUE(star.foundIn("abc"));
    EXPECT_EQ(regexSpans("x*", "axxb"), (std::vector<Span>{{1, 3}}));
    EXPECT_EQ(regexSpans("x*", "\xC3\xA9x"), (std::vector<Span>{{2, 3}}));  // steps over é whole
}

TEST(RegexMatcher, CaseFoldingIsRe2s) {
    EXPECT_EQ(regexSpans("ab", "AB ab", false), (std::vector<Span>{{0, 2}, {3, 5}}));
    EXPECT_TRUE(regexSpans("ab", "AB", true).empty());
    // Unlike plain text search, RE2 also folds non-ASCII letters.
    EXPECT_EQ(regexSpans("\xC3\xA9", "\xC3\x89", false), (std::vector<Span>{{0, 2}}));
    EXPECT_EQ(regexSpans("k", "\xE2\x84\xAA", false), (std::vector<Span>{{0, 3}}));  // KELVIN SIGN
}

TEST(RegexMatcher, BadPatternsAreRejected) {
    EXPECT_THROW(RegexMatcher("(ab", true), std::invalid_argument);
    EXPECT_THROW(RegexMatcher("a)(b", true), std::invalid_argument);
    EXPECT_EQ(RegexMatcher::problemWith("a+"), std::nullopt);
    EXPECT_NE(RegexMatcher::problemWith("[z-a]"), std::nullopt);
}

TEST(Evaluator, RegexesInQueries) {
    const Evaluator evaluator(allOf({regex("^func \\w+"), pathRegex("\\.go$")}), {});
    EXPECT_TRUE(evaluator.matches("a.go", "x\nfunc main"));
    EXPECT_FALSE(evaluator.matches("a.go", "x func main"));
    EXPECT_FALSE(evaluator.matches("a.py", "func main"));
    EXPECT_EQ(evaluator.collectSpans("a.go", "x\nfunc main"), (std::vector<Span>{{2, 11}}));
}

}  // namespace
}  // namespace codesearch
