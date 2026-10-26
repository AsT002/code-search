#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "codesearch/query/ast.hpp"
#include "codesearch/search/literal_matcher.hpp"

namespace codesearch {

// Checks one file's content against the whole query. Build it once per search
// and reuse it for every file.
class Evaluator {
public:
    Evaluator(const QueryNode& query, const SearchOptions& options);

    // Whether the file matches the query.
    bool matches(std::string_view content) const;

    // What to highlight: sorted, without duplicates. Call it only for files
    // where matches() is true.
    std::vector<Span> collectSpans(std::string_view content) const;

private:
    // Same shape as the query tree, with a matcher ready for each literal.
    struct Node {
        std::optional<LiteralMatcher> literal;  // set for literal nodes
        std::vector<Node> children;             // used by AND nodes
    };

    static Node compile(const QueryNode& query, const SearchOptions& options);
    static bool nodeMatches(const Node& node, std::string_view content);
    static void collect(const Node& node, std::string_view content, std::vector<Span>& out);

    Node root_;
};

}  // namespace codesearch
