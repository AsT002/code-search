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
#include "codesearch/util/thread_pool.hpp"

namespace codesearch {

// One line of a result: a line with a match, or a context line around one.
struct ResultLine {
    std::size_t number = 0;     // starts at 1
    std::string text;           // without the line ending, and without a CR before it
    std::vector<Span> matches;  // byte ranges inside `text`; empty for a context line
    friend bool operator==(const ResultLine&, const ResultLine&) = default;
};

// Every line of a file, numbered from 1, with no matches: for showing a
// whole file. Lines are split like resultLines does. Empty for an empty file.
std::vector<ResultLine> allLines(std::string_view content);

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
    // Threads to check files on. nullptr checks them all on this thread.
    ThreadPool* pool = nullptr;
    // About the most bytes of results the pool may hold that the caller
    // hasn't taken yet (see ParallelSearch). Tests make it small, so they
    // can go past it with little data.
    std::size_t maxHeldBytes = std::size_t{32} << 20U;  // 32 MiB
    // If set, a named repo whose index turns out to be damaged is skipped:
    // this is called with the error, the repo's results stop there, and the
    // search goes on with the next repo. Called on the searching thread, in
    // the order results come out. If not set, the error is thrown.
    std::function<void(const RepoIndexError& error)> onDamaged;
};

// Searches each target in turn and calls onFile for each matching file:
// repo by repo, in the order given, then by doc ID. Each repo gets the query
// with its repo: filters resolved, and is skipped if that makes it false.
// Stops as soon as the limit is passed, so memory stays small. If a
// target's index turns out to be damaged, calls request.onDamaged and skips
// the rest of that repo, or without onDamaged throws RepoIndexError naming
// it (or the IndexFileError as it is, if the name is "").
SearchStatus searchRepos(std::span<const SearchTarget> targets, const SearchRequest& request,
                         const std::function<void(const ResultFile& file)>& onFile);

}  // namespace codesearch
