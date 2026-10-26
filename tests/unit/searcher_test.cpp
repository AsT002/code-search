#include "codesearch/search/searcher.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "codesearch/index/memory_index.hpp"

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

}  // namespace
}  // namespace codesearch
