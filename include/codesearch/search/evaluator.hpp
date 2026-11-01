#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/index/index_reader.hpp"
#include "codesearch/query/ast.hpp"
#include "codesearch/search/literal_matcher.hpp"

namespace codesearch {

// Checks one file against the whole query. Build it once per repo and reuse
// it for every file. The query's repo: filters must already be resolved
// (resolveRepo); a RepoNode makes the constructor throw std::logic_error.
class Evaluator {
public:
    Evaluator(const QueryNode& query, const SearchOptions& options);

    // Lets the evaluator use `index` to skip reading text. For a literal
    // that not every candidate must contain (one under a '-' or in an or
    // branch), it looks up which files have all of its trigrams. A file
    // without them can't contain it, so it isn't searched for there. Call it
    // once, before any search, then pass each file's doc ID below.
    void useIndex(const IndexReader& index);

    // Whether the file matches the query.
    bool matches(std::string_view path, std::string_view content) const;
    // The same for file `doc` of the index given to useIndex.
    bool matches(DocId doc, std::string_view path, std::string_view content) const;

    // What to highlight: every match of the text in the parts of the query
    // that matched, sorted, without duplicates. Text under a '-', and text in
    // an or-branch that didn't match, isn't highlighted. Call it only for
    // files where matches() is true.
    std::vector<Span> collectSpans(std::string_view path, std::string_view content) const;
    std::vector<Span> collectSpans(DocId doc, std::string_view path,
                                   std::string_view content) const;

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
        std::string text;                   // for Literal: the text to find
        // For Literal: every candidate of the whole query contains its
        // trigrams, because the query needs it in every matching file.
        bool required = false;
        // For Literal, after useIndex: the files that might contain it,
        // sorted. Only used if `usesIndex`.
        std::vector<DocId> mightContain;
        bool usesIndex = false;
        std::string lang;            // for Lang
        bool value = true;           // for Const
        std::vector<Node> children;  // for And, Or, and Not (one child)
    };

    // The file being checked. `doc` is only set when the index can help.
    struct File {
        std::optional<DocId> doc;
        std::string_view path;
        std::string_view content;
    };

    static Node compile(const QueryNode& query, const SearchOptions& options, bool required);
    static void lookUp(Node& node, const IndexReader& index);
    static bool nodeMatches(const Node& node, const File& file);
    static void collect(const Node& node, const File& file, std::vector<Span>& out);

    Node root_;
};

}  // namespace codesearch
