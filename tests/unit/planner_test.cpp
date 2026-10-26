#include "codesearch/query/planner.hpp"

#include <gtest/gtest.h>

namespace codesearch {
namespace {

TrigramQuery term(char a, char b, char c) {
    return {TrigramTerm{packTrigram(a, b, c)}};
}

TEST(Planner, ShortLiteralMatchesEverything) {
    EXPECT_EQ(planCandidates(literal("")), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(literal("ab")), TrigramQuery{AllQuery{}});
}

TEST(Planner, ThreeByteLiteralIsOneTerm) {
    EXPECT_EQ(planCandidates(literal("abc")), term('a', 'b', 'c'));
}

TEST(Planner, LongerLiteralIsAndOfTrigrams) {
    const TrigramQuery expected{AndQuery{{term('a', 'b', 'c'), term('b', 'c', 'd')}}};
    EXPECT_EQ(planCandidates(literal("abcd")), expected);
}

TEST(Planner, UsesFoldedTrigrams) {
    EXPECT_EQ(planCandidates(literal("ABC")), term('a', 'b', 'c'));
}

TEST(Planner, AndFlattensAndDropsAll) {
    const QueryNode query = allOf({literal("abc"), literal("x"), allOf({literal("xyz")})});
    const TrigramQuery expected{AndQuery{{term('a', 'b', 'c'), term('x', 'y', 'z')}}};
    EXPECT_EQ(planCandidates(query), expected);
}

TEST(Planner, AndOfShortLiteralsMatchesEverything) {
    EXPECT_EQ(planCandidates(allOf({literal("a"), literal("bc")})), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(allOf({})), TrigramQuery{AllQuery{}});
}

}  // namespace
}  // namespace codesearch
