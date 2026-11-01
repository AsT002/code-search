#include "codesearch/query/parser.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace codesearch {
namespace {

QueryNode parsed(const std::string& text) {
    return parseQuery(text).root;
}

// Expects parseQuery to throw QueryError with `part` in the message.
void expectError(const std::string& text, const std::string& part) {
    try {
        parseQuery(text);
        ADD_FAILURE() << "expected an error for: " << text;
    } catch (const QueryError& error) {
        EXPECT_NE(std::string(error.what()).find(part), std::string::npos)
            << "query: " << text << "\ngot: " << error.what() << "\nwanted: " << part;
    }
}

TEST(Parser, WordsAreAnded) {
    EXPECT_EQ(parsed("foo"), literal("foo"));
    EXPECT_EQ(parsed("  foo   bar\t"), allOf({literal("foo"), literal("bar")}));
}

TEST(Parser, OrBindsLooserThanAnd) {
    EXPECT_EQ(parsed("a b or c"), anyOf({allOf({literal("a"), literal("b")}), literal("c")}));
    EXPECT_EQ(parsed("a OR b or c"), anyOf({literal("a"), literal("b"), literal("c")}));
    EXPECT_EQ(parsed("a (b or c)"), allOf({literal("a"), anyOf({literal("b"), literal("c")})}));
    EXPECT_EQ(parsed("Or"), literal("Or"));  // only "or" and "OR" are the keyword
}

TEST(Parser, MinusExcludes) {
    EXPECT_EQ(parsed("a -b"), allOf({literal("a"), negate(literal("b"))}));
    EXPECT_EQ(parsed("-(a or b)"), negate(anyOf({literal("a"), literal("b")})));
    EXPECT_EQ(parsed("-path:x"), negate(pathFilter("x")));
    EXPECT_EQ(parsed("-or"), negate(literal("or")));
    EXPECT_EQ(parsed("-(-a)"), negate(negate(literal("a"))));
    // A dash on its own, or before another dash, is text.
    EXPECT_EQ(parsed("a - b"), allOf({literal("a"), literal("-"), literal("b")}));
    EXPECT_EQ(parsed("--verbose"), literal("--verbose"));
    EXPECT_EQ(parsed("a-b"), literal("a-b"));
}

TEST(Parser, QuotesAndBackslashes) {
    EXPECT_EQ(parsed("\"foo bar\""), literal("foo bar"));
    EXPECT_EQ(parsed("\"or\""), literal("or"));
    // Text that's "or" once quotes and escapes are gone is still text.
    EXPECT_EQ(parsed("a or\"\" b"), allOf({literal("a"), literal("or"), literal("b")}));
    EXPECT_EQ(parsed("a o\\r b"), allOf({literal("a"), literal("or"), literal("b")}));
    EXPECT_EQ(parsed("a \"\"OR b"), allOf({literal("a"), literal("OR"), literal("b")}));
    EXPECT_EQ(parsed("\"-x\""), literal("-x"));
    EXPECT_EQ(parsed(R"("say \"hi\" \\ \d")"), literal(R"(say "hi" \ \d)"));
    EXPECT_EQ(parsed("pre\"fix mid\"dle"), literal("prefix middle"));  // joined like a shell
    EXPECT_EQ(parsed(R"(\-x \(y\) a\ b \")"),
              allOf({literal("-x"), literal("(y)"), literal("a b"), literal("\"")}));
}

TEST(Parser, ParenthesesInsideWordsAreText) {
    EXPECT_EQ(parsed("printf("), literal("printf("));
    EXPECT_EQ(parsed("f(x)"), literal("f(x)"));
    EXPECT_EQ(parsed("(f(x) or g)"), anyOf({literal("f(x)"), literal("g")}));
    EXPECT_EQ(parsed("(a)b"), allOf({literal("a"), literal("b")}));
}

TEST(Parser, Filters) {
    EXPECT_EQ(parsed("repo:backend"), repoFilter("backend"));
    EXPECT_EQ(parsed("path:src/ file:.go"), allOf({pathFilter("src/"), pathFilter(".go")}));
    EXPECT_EQ(parsed("path:\"my dir\""), pathFilter("my dir"));
    EXPECT_EQ(parsed("lang:Go lang:c++ lang:js"),
              allOf({langFilter("go"), langFilter("cpp"), langFilter("javascript")}));
    EXPECT_EQ(parsed("path:a:b"), pathFilter("a:b"));
    // Not a filter: unknown names, and names in quotes or after a backslash.
    EXPECT_EQ(parsed("std::string"), literal("std::string"));
    EXPECT_EQ(parsed("http://x"), literal("http://x"));
    EXPECT_EQ(parsed("\"path:x\""), literal("path:x"));
    EXPECT_EQ(parsed("pa\\th:x"), literal("path:x"));
}

TEST(Parser, Regexes) {
    EXPECT_EQ(parsed("/auth(enticate|orize)/"), regex("auth(enticate|orize)"));
    EXPECT_EQ(parsed(R"(/\bfoo\d+\./)"), regex(R"(\bfoo\d+\.)"));  // backslashes stay, for RE2
    EXPECT_EQ(parsed(R"(/a\/b/)"), regex("a/b"));                  // except \/, for /
    EXPECT_EQ(parsed("/a b/ c"), allOf({regex("a b"), literal("c")}));
    EXPECT_EQ(parsed("(/x/ or y)"), anyOf({regex("x"), literal("y")}));
    EXPECT_EQ(parsed("-/x/"), negate(regex("x")));
    EXPECT_EQ(parsed(R"(path:/\.go$/ file:/^src/)"),
              allOf({pathRegex(R"(\.go$)"), pathRegex("^src")}));
    // Not regexes: no closing / before a space or the end, or nothing inside.
    EXPECT_EQ(parsed("/usr/bin"), literal("/usr/bin"));
    EXPECT_EQ(parsed("// TODO"), allOf({literal("//"), literal("TODO")}));
    EXPECT_EQ(parsed("a/b/"), literal("a/b/"));
    EXPECT_EQ(parsed("\"/x/\""), literal("/x/"));
}

TEST(Parser, BadRegexesAreErrors) {
    expectError("/(ab/", "the regex /(ab/ isn't valid: missing )");
    expectError("/a)(b/", "isn't valid");  // valid only if wrapped in a group
    expectError("path:/[z-a]/", "isn't valid");
}

TEST(Parser, CaseIsAnOption) {
    ParsedQuery query = parseQuery("foo case:yes");
    EXPECT_EQ(query.root, literal("foo"));
    EXPECT_EQ(query.caseSensitive, true);
    query = parseQuery("(a or b case:no)");
    EXPECT_EQ(query.root, anyOf({literal("a"), literal("b")}));
    EXPECT_EQ(query.caseSensitive, false);
    EXPECT_FALSE(parseQuery("foo").caseSensitive.has_value());
    EXPECT_EQ(parseQuery("case:yes a case:yes").caseSensitive, true);
}

TEST(Parser, Errors) {
    expectError("", "the query is empty");
    expectError("   ", "the query is empty");
    expectError("or", "\"or\" needs something on both sides");
    expectError("a or", "\"or\" needs something on both sides");
    expectError("a or or b", "\"or\" needs something on both sides");
    expectError("()", "nothing inside ( )");
    expectError("(a", "a ( is missing its )");
    expectError("a)", "a ) without a matching (");
    expectError("-)", "a ) without a matching (");
    expectError("\"abc", "missing its closing \"");
    expectError("abc\\", "nothing to make plain");
    expectError("\"\"", "\"\" is empty");
    expectError("path:", "path: needs a value");
    expectError("repo:\"\"", "repo: needs a value");
    expectError("lang:cobol", "unknown language \"cobol\". Known: c, cmake, cpp");
    expectError("case:maybe", "case: takes yes or no");
    expectError("case:yes case:no", "both case:yes and case:no");
    expectError("-case:yes a", "case: can't have a - in front of it");
    expectError("case:yes", "case: needs something to search for");
    expectError("a or case:yes", "case: needs something to search for");
}

TEST(Parser, LimitsNesting) {
    const std::string ok = std::string(100, '(') + "a" + std::string(100, ')');
    EXPECT_EQ(parsed(ok), literal("a"));
    expectError(std::string(101, '(') + "a" + std::string(101, ')'), "nested too deeply");

    std::string dashes;
    for (int i = 0; i < 101; ++i) {
        dashes += "-(";
    }
    expectError(dashes + "a", "nested too deeply");
    expectError(std::string(100000, '('), "nested too deeply");
}

TEST(Parser, LongQueriesWork) {
    std::string text;
    for (int i = 0; i < 20000; ++i) {
        text += "w" + std::to_string(i) + (i % 2 == 0 ? " " : " or ");
    }
    text += "end";
    EXPECT_NO_THROW(parseQuery(text));
}

// Random text made of the characters the parser treats specially. Every
// query must parse or throw QueryError: no other exception, no crash.
TEST(Parser, RandomInputNeverCrashes) {
    const std::string alphabet = "ab -()\"\\:orOR\t\xC3\xA9";
    const std::vector<std::string> words = {"path:", "repo:", "lang:go", "case:", "or ", "-("};
    // A fixed seed, so a failure comes back the same way.
    std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    for (int round = 0; round < 20000; ++round) {
        std::string text;
        const std::size_t length = std::uniform_int_distribution<std::size_t>(0, 30)(rng);
        for (std::size_t i = 0; i < length; ++i) {
            if (std::uniform_int_distribution<int>(0, 5)(rng) == 0) {
                text += words[std::uniform_int_distribution<std::size_t>(0, words.size() - 1)(rng)];
            } else {
                text += alphabet[std::uniform_int_distribution<std::size_t>(
                    0, alphabet.size() - 1)(rng)];
            }
        }
        try {
            parseQuery(text);
        } catch (const QueryError&) {  // NOLINT(bugprone-empty-catch) an error is fine
        }
    }
}

}  // namespace
}  // namespace codesearch
