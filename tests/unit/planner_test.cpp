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

TEST(Planner, OrOfLiterals) {
    const TrigramQuery expected{OrQuery{{term('a', 'b', 'c'), term('x', 'y', 'z')}}};
    EXPECT_EQ(planCandidates(anyOf({literal("abc"), literal("xyz")})), expected);
    // One side that could be anything makes the whole or anything.
    EXPECT_EQ(planCandidates(anyOf({literal("abc"), literal("x")})), TrigramQuery{AllQuery{}});
    // Nested ors are lifted up.
    EXPECT_EQ(planCandidates(anyOf({literal("abc"), anyOf({literal("xyz")})})), expected);
}

TEST(Planner, FiltersAndNotMatchEverything) {
    EXPECT_EQ(planCandidates(negate(literal("abc"))), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(pathFilter("abc")), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(langFilter("go")), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(allOf({literal("abc"), negate(literal("xyz"))})), term('a', 'b', 'c'));
    EXPECT_EQ(planCandidates(constant(true)), TrigramQuery{AllQuery{}});
    EXPECT_EQ(planCandidates(constant(false)), TrigramQuery{NoneQuery{}});
    EXPECT_EQ(planCandidates(anyOf({constant(false), literal("abc")})), term('a', 'b', 'c'));
}

TEST(Simplify, RemovesKnownParts) {
    const QueryNode a = literal("a");
    const QueryNode b = literal("b");
    EXPECT_EQ(simplify(allOf({a, constant(true), b})), allOf({a, b}));
    EXPECT_EQ(simplify(allOf({a, constant(false)})), constant(false));
    EXPECT_EQ(simplify(anyOf({a, constant(false)})), a);
    // Every file matches, but a still gets highlighted where it's found.
    EXPECT_EQ(simplify(anyOf({constant(true), a})), anyOf({a, constant(true)}));
    EXPECT_EQ(simplify(anyOf({constant(true), anyOf({a, constant(true)})})),
              anyOf({a, constant(true)}));
    EXPECT_EQ(simplify(anyOf({constant(true), constant(false)})), constant(true));
    EXPECT_EQ(simplify(negate(constant(true))), constant(false));
    EXPECT_EQ(simplify(allOf({constant(true)})), constant(true));
    EXPECT_EQ(simplify(anyOf({constant(false)})), constant(false));
    EXPECT_EQ(simplify(allOf({a, allOf({b, constant(true)})})), allOf({a, b}));
    EXPECT_EQ(simplify(anyOf({a, anyOf({b})})), anyOf({a, b}));
    EXPECT_EQ(simplify(negate(allOf({a, constant(true)}))), negate(a));
}

TEST(ResolveRepo, KeepsTheWholeQueryPerRepo) {
    // (repo:A foo) or (repo:B bar): A must only find foo, B only bar.
    const QueryNode query =
        anyOf({allOf({repoFilter("A"), literal("foo")}), allOf({repoFilter("B"), literal("bar")})});
    EXPECT_EQ(resolveRepo(query, "A"), literal("foo"));
    EXPECT_EQ(resolveRepo(query, "B"), literal("bar"));
    EXPECT_EQ(resolveRepo(query, "C"), constant(false));

    EXPECT_EQ(resolveRepo(allOf({negate(repoFilter("A")), literal("x")}), "A"), constant(false));
    EXPECT_EQ(resolveRepo(allOf({negate(repoFilter("A")), literal("x")}), "B"), literal("x"));
    EXPECT_EQ(resolveRepo(literal("x"), "A"), literal("x"));
}

TEST(RepoNamesIn, SortedOnce) {
    const QueryNode query = anyOf(
        {repoFilter("b"), negate(repoFilter("a")), allOf({repoFilter("b"), literal("repo:c")})});
    EXPECT_EQ(repoNamesIn(query), (std::vector<std::string>{"a", "b"}));
    EXPECT_TRUE(repoNamesIn(literal("x")).empty());
}

}  // namespace
}  // namespace codesearch
