#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/search/evaluator.hpp"
#include "codesearch/search/literal_matcher.hpp"
#include "codesearch/search/regex_matcher.hpp"

#include <re2/re2.h>

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

// Non-overlapping matches by comparing at every position, the plain way.
std::vector<Span> plainScan(std::string_view pattern, std::string_view text, bool caseSensitive) {
    const auto same = [&](char a, char b) {
        if (!caseSensitive) {
            a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
            b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
        }
        return a == b;
    };
    std::vector<Span> spans;
    std::size_t i = 0;
    while (i + pattern.size() <= text.size()) {
        std::size_t j = 0;
        while (j < pattern.size() && same(text[i + j], pattern[j])) {
            ++j;
        }
        if (j == pattern.size()) {
            spans.push_back(Span{i, i + j});
            i += j;
        } else {
            ++i;
        }
    }
    return spans;
}

// Patterns over 64 bytes take another route. Check it against a plain scan
// on text made of pieces of the pattern, where partial matches are common.
TEST(LiteralMatcher, LongPatternsMatchAPlainScan) {
    // A fixed seed, so a failure comes back the same way.
    std::mt19937 rng(5);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    const auto number = [&](std::size_t lo, std::size_t hi) {
        return std::uniform_int_distribution<std::size_t>(lo, hi)(rng);
    };
    constexpr std::string_view kAlphabet = "aAb";
    for (int round = 0; round < 2000; ++round) {
        std::string pattern;
        for (std::size_t n = number(65, 72); n > 0; --n) {
            pattern += kAlphabet[number(0, 2)];
        }
        std::string text;
        while (text.size() < 300) {
            const std::size_t from = number(0, pattern.size() - 1);
            text += pattern.substr(from, number(1, pattern.size() - from));
            if (number(0, 3) == 0) {
                text += kAlphabet[number(0, 2)];
            }
        }
        for (const bool caseSensitive : {true, false}) {
            ASSERT_EQ(findAll(pattern, text, caseSensitive),
                      plainScan(pattern, text, caseSensitive))
                << pattern << " in " << text << (caseSensitive ? "" : ", ignoring case");
        }
    }
}

// For "aaa...b...aaa" in "aaa...", every position passes the test of the
// first and last bytes. Comparing the pattern at each one would take time
// growing with text size times pattern size: minutes here.
TEST(LiteralMatcher, RepetitiveTextStaysFast) {
    const std::string text(std::size_t{1} << 20U, 'a');
    const std::string half(std::size_t{1} << 15U, 'a');
    const std::string pattern = half + "b" + half;
    for (const bool caseSensitive : {true, false}) {
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(findAll(pattern, text, caseSensitive).empty());
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_LT(seconds, 10.0);
    }
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

// \Q quotes the rest of the pattern. A group around the pattern would
// have its ) quoted too, and the regex would be broken.
TEST(RegexMatcher, QuoteToTheEnd) {
    EXPECT_EQ(regexSpans("\\Qa.b", "axb a.b"), (std::vector<Span>{{4, 7}}));
    EXPECT_EQ(regexSpans("x|\\Qy)", "y) x"), (std::vector<Span>{{0, 2}, {3, 4}}));
}

// A match never takes in a line ending, like grep: \n never matches, and
// [^x]+ stops at the end of each line.
TEST(RegexMatcher, MatchesStayWithinALine) {
    EXPECT_TRUE(regexSpans("a\\nb", "a\nb").empty());
    EXPECT_FALSE(RegexMatcher("a\\nb", true).foundIn("a\nb"));
    EXPECT_EQ(regexSpans("[^x]+", "ab\ncd"), (std::vector<Span>{{0, 2}, {3, 5}}));
}

// \C matches any one byte, and never_nl doesn't stop it at a '\n', so a
// regex with \C is searched one line at a time. ^, $ and \b still see the
// lines around it.
TEST(RegexMatcher, AnyByteStaysWithinALine) {
    EXPECT_FALSE(RegexMatcher("a\\Cb", true).foundIn("a\nb"));
    EXPECT_TRUE(regexSpans("a\\Cb", "a\nb").empty());
    EXPECT_TRUE(RegexMatcher("a\\Cb", true).foundIn("a\nb\naxb"));
    EXPECT_EQ(regexSpans("a\\Cb", "a\nb\naxb"), (std::vector<Span>{{4, 7}}));
    EXPECT_EQ(regexSpans("\\C+", "ab\ncd"), (std::vector<Span>{{0, 2}, {3, 5}}));
    EXPECT_EQ(regexSpans("\\C$", "ab\ncd"), (std::vector<Span>{{1, 2}, {4, 5}}));
    EXPECT_EQ(regexSpans("^\\C", "ab\ncd"), (std::vector<Span>{{0, 1}, {3, 4}}));
    EXPECT_EQ(regexSpans("\\C\\b", "ab cd"), (std::vector<Span>{{1, 2}, {2, 3}, {4, 5}}));
    // A \C match can start inside a character: after the empty match at
    // the start of é, the next one starts at é's second byte.
    EXPECT_EQ(regexSpans("\\C?z|", "\xC3\xA9z"), (std::vector<Span>{{1, 3}}));
}

// Finding every match could take time growing with the square of a line's
// length: for /a*z|/ on a line of a's, each search looks to the line's end
// for a z. findAll stops after a time limit, so this takes a fraction of a
// second, not minutes.
TEST(RegexMatcher, FindingAllMatchesStaysFast) {
    const std::string line(200000, 'a');
    for (const std::string_view pattern : {"a*z|", "a*z|a"}) {
        SCOPED_TRACE(pattern);
        const auto start = std::chrono::steady_clock::now();
        regexSpans(pattern, line);
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_LT(seconds, 20.0);
    }
    // The same with \C, whose searches are held to their line, over many
    // short lines.
    std::string shortLines;
    for (int i = 0; i < 20000; ++i) {
        shortLines += "aaaaaaaaa\n";
    }
    for (const std::string_view pattern : {"\\C*z|", "\\C*z|a"}) {
        SCOPED_TRACE(pattern);
        const auto start = std::chrono::steady_clock::now();
        regexSpans(pattern, shortLines);
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_LT(seconds, 20.0);
    }
}

// Ordinary regexes highlight every match, however long the line, like
// plain text does. (findAll's time limit allows 1 microsecond per byte; a
// release build takes a twentieth of that even with a match on every
// byte, but sanitizer builds are much slower, so these are less dense.)
TEST(RegexMatcher, LongLinesKeepEveryMatch) {
    std::string words;
    for (int i = 0; i < 1000; ++i) {
        words += "foo ";
    }
    EXPECT_EQ(regexSpans("foo", words).size(), 1000U);
    std::string list = "var x=[0";
    for (int i = 1; i < 2000; ++i) {
        list += "," + std::to_string(i);
    }
    EXPECT_EQ(regexSpans(",", list).size(), 1999U);
    EXPECT_EQ(regexSpans("\\d+", list).size(), 2000U);
    std::string sparse;
    for (int i = 0; i < 10000; ++i) {
        sparse += "xxxxxxxxxe";
    }
    EXPECT_EQ(regexSpans("e", sparse).size(), 10000U);
}

// RE2's default 8 MiB is too little for patterns like
// /(a|b).{1000}(a|b){1000}c/: on 1 MiB, 17 s instead of 0.06 s with 64 MiB
// in a release build. Checked by the setting, since building that much
// fast-matcher state takes long in sanitizer builds.
TEST(RegexMatcher, BigPatternsGetEnoughMemory) {
    EXPECT_EQ(RegexMatcher("a", true).compiled().options().max_mem(),
              std::int64_t{64} * 1024 * 1024);
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
