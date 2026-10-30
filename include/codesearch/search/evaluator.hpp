#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "codesearch/query/ast.hpp"
#include "codesearch/search/literal_matcher.hpp"

namespace codesearch {

// Checks one file against the whole query. Build it once per repo and reuse
// it for every file. The query's repo: filters must already be resolved
// (resolveRepo); a RepoNode makes the constructor throw std::logic_error.
class Evaluator {
public:
    Evaluator(const QueryNode& query, const SearchOptions& options);

    // Whether the file matches the query.
    bool matches(std::string_view path, std::string_view content) const;

    // What to highlight: every match of the text in the parts of the query
    // that matched, sorted, without duplicates. Text under a '-', and text in
    // an or-branch that didn't match, isn't highlighted. Call it only for
    // files where matches() is true.
    std::vector<Span> collectSpans(std::string_view path, std::string_view content) const;

private:
    enum class Kind {
        Literal,
        Path,
        Lang,
        Const,
        And,
        Or,
        Not,
    };

    // Same shape as the query tree, with a matcher ready for text and paths.
    struct Node {
        Kind kind = Kind::Const;
        LiteralMatcher matcher{"", false};  // for Literal and Path
        std::string lang;                   // for Lang
        bool value = true;                  // for Const
        std::vector<Node> children;         // for And, Or, and Not (one child)
    };

    static Node compile(const QueryNode& query, const SearchOptions& options);
    static bool nodeMatches(const Node& node, std::string_view path, std::string_view content);
    static void collect(const Node& node, std::string_view path, std::string_view content,
                        std::vector<Span>& out);

    Node root_;
};

}  // namespace codesearch
