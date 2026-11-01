#include "codesearch/search/candidates.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "codesearch/index/memory_index.hpp"
#include "codesearch/query/planner.hpp"

namespace codesearch {
namespace {

TEST(PostingCursor, WalksAndSkips) {
    const std::vector<DocId> docs = {1, 4, 7, 9};
    PostingCursor cursor(docs);
    EXPECT_EQ(cursor.size(), 4U);
    EXPECT_EQ(cursor.doc(), 1U);

    cursor.next();
    EXPECT_EQ(cursor.doc(), 4U);

    cursor.advance(5);
    EXPECT_EQ(cursor.doc(), 7U);

    cursor.advance(2);  // never goes back
    EXPECT_EQ(cursor.doc(), 7U);

    cursor.advance(10);
    EXPECT_TRUE(cursor.done());
}

TEST(MemoryIndex, ListsTrigramsSorted) {
    IndexBuilder builder;
    builder.add("0", "cab");
    builder.add("1", "abc");
    const MemoryIndex index = builder.build();
    EXPECT_EQ(index.trigrams(),
              (std::vector<Trigram>{packTrigram('a', 'b', 'c'), packTrigram('c', 'a', 'b')}));
}

MemoryIndex sampleIndex() {
    IndexBuilder builder;
    builder.add("0", "hello world");
    builder.add("1", "help me");
    builder.add("2", "say hello");
    builder.add("3", "nothing here");
    return builder.build();
}

TEST(IndexBuilder, AssignsIdsInOrder) {
    IndexBuilder builder;
    EXPECT_EQ(builder.add("a", "x"), 0U);
    EXPECT_EQ(builder.add("b", "y"), 1U);
    const MemoryIndex index = builder.build();
    EXPECT_EQ(index.docCount(), 2U);
    EXPECT_EQ(index.path(1), "b");
}

TEST(FindCandidates, AllAndNone) {
    const MemoryIndex index = sampleIndex();
    EXPECT_EQ(findCandidates(index, {AllQuery{}}), (std::vector<DocId>{0, 1, 2, 3}));
    EXPECT_TRUE(findCandidates(index, {NoneQuery{}}).empty());
}

TEST(FindCandidates, SingleTrigram) {
    const MemoryIndex index = sampleIndex();
    const TrigramQuery query{TrigramTerm{packTrigram('h', 'e', 'l')}};
    EXPECT_EQ(findCandidates(index, query), (std::vector<DocId>{0, 1, 2}));
}

TEST(FindCandidates, IntersectsLiteralTrigrams) {
    const MemoryIndex index = sampleIndex();
    EXPECT_EQ(findCandidates(index, planCandidates(literal("hello"))), (std::vector<DocId>{0, 2}));
}

TEST(FindCandidates, MissingTrigramMeansNoDocs) {
    const MemoryIndex index = sampleIndex();
    EXPECT_TRUE(findCandidates(index, planCandidates(literal("zzz"))).empty());
    EXPECT_TRUE(findCandidates(index, planCandidates(literal("hello zzz"))).empty());
}

TEST(FindCandidates, IsCaseInsensitive) {
    const MemoryIndex index = sampleIndex();
    EXPECT_EQ(findCandidates(index, planCandidates(literal("HELLO"))), (std::vector<DocId>{0, 2}));
}

TEST(FindCandidates, NestedAnd) {
    const MemoryIndex index = sampleIndex();
    const TrigramQuery inner{AndQuery{{{TrigramTerm{packTrigram('h', 'e', 'l')}}}}};
    const TrigramQuery outer{AndQuery{{inner, {TrigramTerm{packTrigram('s', 'a', 'y')}}}}};
    EXPECT_EQ(findCandidates(index, outer), (std::vector<DocId>{2}));
}

TEST(FindCandidates, OrMergesWithoutRepeats) {
    const MemoryIndex index = sampleIndex();
    const TrigramQuery query =
        planCandidates(anyOf({literal("hello"), literal("say"), literal("zzz")}));
    // "hello" is in 0 and 2, "say" in 2, "zzz" nowhere.
    EXPECT_EQ(findCandidates(index, query), (std::vector<DocId>{0, 2}));
    EXPECT_TRUE(findCandidates(index, {OrQuery{}}).empty());
}

TEST(FindCandidates, OrInsideAnd) {
    const MemoryIndex index = sampleIndex();
    const QueryNode query = allOf({literal("hello"), anyOf({literal("say"), literal("zzz")})});
    EXPECT_EQ(findCandidates(index, planCandidates(query)), (std::vector<DocId>{2}));
}

// Once few candidates are left, a much longer list isn't walked: checking
// those files is cheaper. So a doc the long list would rule out can stay.
TEST(FindCandidates, StopsBeforeAListThatIsntWorthWalking) {
    IndexBuilder builder;
    for (int i = 0; i < 2000; ++i) {
        builder.add(std::to_string(i), i == 5 ? "rare" : "common");
    }
    const MemoryIndex index = builder.build();
    const auto term = [](std::string_view three) {
        return TrigramQuery{TrigramTerm{packTrigram(three[0], three[1], three[2])}};
    };
    // "rar" is only in doc 5, and "com" is in every doc but 5.
    const TrigramQuery both{AndQuery{{term("rar"), term("com")}}};
    EXPECT_EQ(findCandidates(index, both), (std::vector<DocId>{5}));

    // With lists of similar length, it intersects as usual.
    IndexBuilder small;
    small.add("a", "rare");
    small.add("b", "common");
    EXPECT_TRUE(findCandidates(small.build(), both).empty());
}

}  // namespace
}  // namespace codesearch
