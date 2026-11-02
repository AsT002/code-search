#include "codesearch/search/searcher.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "codesearch/index/memory_index.hpp"
#include "codesearch/query/planner.hpp"

namespace codesearch {
namespace {

TEST(Locate, CountsLinesAndColumns) {
    const std::vector<Span> spans = {{0, 1}, {4, 5}, {9, 10}};
    const std::vector<Match> expected = {
        {.bytes = {0, 1}, .line = 1, .column = 1},
        {.bytes = {4, 5}, .line = 2, .column = 1},
        {.bytes = {9, 10}, .line = 4, .column = 2},
    };
    EXPECT_EQ(locate("ab\r\ncd\n\nxy", spans), expected);
}

TEST(Search, FindsMatchesWithPositions) {
    IndexBuilder builder;
    builder.add("a.cpp", "int main() {\n  return authenticate();\n}\n");
    builder.add("b.cpp", "// nothing to see\n");
    builder.add("c.cpp", "AUTHENTICATE\n");
    const MemoryIndex index = builder.build();

    const SearchResults results = search(index, literal("authenticate"));

    ASSERT_EQ(results.files.size(), 2U);
    EXPECT_EQ(results.files[0].path, "a.cpp");
    EXPECT_EQ(results.files[0].matches,
              (std::vector<Match>{{.bytes = {22, 34}, .line = 2, .column = 10}}));
    EXPECT_EQ(results.files[1].path, "c.cpp");
}

TEST(Search, CaseSensitiveDropsFalseCandidates) {
    IndexBuilder builder;
    builder.add("a", "Hello");
    builder.add("b", "hello");
    const MemoryIndex index = builder.build();

    const SearchResults results = search(index, literal("Hello"), {.caseSensitive = true});

    ASSERT_EQ(results.files.size(), 1U);
    EXPECT_EQ(results.files[0].path, "a");
}

TEST(Search, ShortQueryChecksEveryFile) {
    IndexBuilder builder;
    builder.add("a", "xy");
    builder.add("b", "zz");
    const MemoryIndex index = builder.build();

    const SearchResults results = search(index, literal("y"));

    ASSERT_EQ(results.files.size(), 1U);
    EXPECT_EQ(results.files[0].path, "a");
}

// The index folds only ASCII, but RE2 folds case by Unicode rules, and its
// prefilter lowercases non-ASCII characters, some into ASCII. A regex must
// still find these files: the index step may not rule them out.
std::vector<std::string> regexFinds(const std::string& pattern, std::string_view content,
                                    bool caseSensitive) {
    IndexBuilder builder;
    builder.add("a.txt", std::string(content));
    builder.add("other.txt", "nothing here at all");
    const SearchResults results =
        search(builder.build(), regex(pattern), SearchOptions{.caseSensitive = caseSensitive});
    std::vector<std::string> paths;
    paths.reserve(results.files.size());
    for (const FileMatch& file : results.files) {
        paths.push_back(file.path);
    }
    return paths;
}

TEST(RegexSearch, KelvinSignForK) {
    // Without case, RE2 matches KELVIN SIGN (U+212A) for k, but the index has
    // no "key" in this file.
    EXPECT_EQ(regexFinds("key",
                         "\xE2\x84\xAA"
                         "ey",
                         false),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, NonAsciiCase) {
    // The prefilter lowercases É to é; the index keeps É as it is.
    EXPECT_EQ(regexFinds("\xC3\xA9t\xC3\xA9", "\xC3\x89T\xC3\x89", false),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, PatternsNamingNonAscii) {
    // Even with case: \x{212A} names KELVIN SIGN, which the prefilter
    // lowercases to an ASCII k.
    EXPECT_EQ(regexFinds("[k\\x{212A}]ey",
                         "\xE2\x84\xAA"
                         "ey",
                         true),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, LongSForS) {
    // Without case, RE2 matches LONG S (U+017F) for s.
    EXPECT_EQ(regexFinds("same",
                         "\xC5\xBF"
                         "ame",
                         false),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, CaseFlagsInsideThePattern) {
    // case:yes, but (?i) turns folding on, so KELVIN SIGN matches k again.
    EXPECT_EQ(regexFinds("(?i)key",
                         "\xE2\x84\xAA"
                         "ey",
                         true),
              std::vector<std::string>{"a.txt"});
    EXPECT_EQ(regexFinds("x(?i:key)",
                         "x\xE2\x84\xAA"
                         "ey",
                         true),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, OctalEscapesNamingNonAscii) {
    // \577 is octal for U+017F, LONG S, which the prefilter lowercases to s.
    EXPECT_EQ(regexFinds("\\577ame",
                         "\xC5\xBF"
                         "ame",
                         true),
              std::vector<std::string>{"a.txt"});
}

TEST(RegexSearch, TheIndexStillNarrowsAsciiRegexes) {
    const TrigramQuery plan =
        planCandidates(regex("foo|bar"), SearchOptions{.caseSensitive = true});
    EXPECT_TRUE(std::holds_alternative<OrQuery>(plan.value));
    EXPECT_EQ(regexFinds("fo+ba[rz]", "x foooobaz y", false), std::vector<std::string>{"a.txt"});
}

// Without case, a k or s in the text may be KELVIN SIGN or LONG S, so only
// the text between them is required. That still narrows the search.
TEST(RegexSearch, TextAroundKAndSStillNarrows) {
    const TrigramQuery plan = planCandidates(regex("MustCompile\\(\""), SearchOptions{});
    EXPECT_FALSE(std::holds_alternative<AllQuery>(plan.value));
    EXPECT_EQ(regexFinds("mustcompile",
                         "Mu\xC5\xBF"
                         "tCompile",
                         false),
              std::vector<std::string>{"a.txt"});
    EXPECT_EQ(regexFinds("breakpoint",
                         "brea\xE2\x84\xAA"
                         "point",
                         false),
              std::vector<std::string>{"a.txt"});
}

}  // namespace
}  // namespace codesearch
