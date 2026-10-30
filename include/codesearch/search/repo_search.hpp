#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/index_reader.hpp"
#include "codesearch/query/ast.hpp"
#include "codesearch/search/searcher.hpp"

namespace codesearch {

// One line of a result: a line with a match, or a context line around one.
struct ResultLine {
    std::size_t number = 0;     // starts at 1
    std::string text;           // without the line ending, and without a CR before it
    std::vector<Span> matches;  // byte ranges inside `text`; empty for a context line
    friend bool operator==(const ResultLine&, const ResultLine&) = default;
};

// The lines to show for one file: each line with a match, up to
// `maxMatchLines` of them, plus up to `context` lines before and after each.
// `matches` must be sorted, as locate() gives them. A match that runs past
// the end of its line is cut at the line's end.
std::vector<ResultLine> resultLines(std::string_view content, std::span<const Match> matches,
                                    std::size_t context, std::size_t maxMatchLines);

// One file in the results.
struct ResultFile {
    std::string repo;  // "" when searching a folder that isn't added
    std::string path;
    // Matching lines and their context. Empty with filesOnly, and for a file
    // that matched with nothing to highlight, like with lang:go or -foo.
    std::vector<ResultLine> lines;
    friend bool operator==(const ResultFile&, const ResultFile&) = default;
};

// A damaged index found while searching a repo. what() starts with the
// repo's name.
class RepoIndexError : public IndexFileError {
public:
    RepoIndexError(const std::string& repo, const std::string& message)
        : IndexFileError(repo + ": " + message), repo_(repo) {}

    const std::string& repo() const { return repo_; }

private:
    std::string repo_;
};

// A repo to search, by name.
struct SearchTarget {
    std::string name;
    const IndexReader* index = nullptr;
};

struct SearchRequest {
    QueryNode query;  // the whole query, repo: filters and all
    SearchOptions options;
    // At most this many results; 0 means no limit. A result is a matching
    // line, or a file when there's nothing to highlight or with filesOnly.
    std::size_t maxResults = 0;
    std::size_t contextLines = 0;
    bool filesOnly = false;
    const std::atomic<bool>* cancelled = nullptr;  // checked before each file
};

// Searches each target in turn and calls onFile for each matching file:
// repo by repo, in the order given, then by doc ID. Each repo gets the query
// with its repo: filters resolved, and is skipped if that makes it false.
// Stops as soon as the limit is passed, so memory stays small. If a
// target's index turns out to be damaged, throws RepoIndexError naming it
// (or the IndexFileError as it is, if the name is "").
SearchStatus searchRepos(std::span<const SearchTarget> targets, const SearchRequest& request,
                         const std::function<void(const ResultFile& file)>& onFile);

}  // namespace codesearch
