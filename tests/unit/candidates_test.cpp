#include "codesearch/search/candidates.hpp"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace codesearch
