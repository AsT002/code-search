#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/index/index_reader.hpp"
#include "codesearch/query/ast.hpp"
#include "codesearch/search/literal_matcher.hpp"

namespace codesearch {

struct Match {
    Span bytes;
    std::size_t line = 0;    // starts at 1
    std::size_t column = 0;  // starts at 1, counted in bytes
    friend bool operator==(const Match&, const Match&) = default;
};

struct FileMatch {
    DocId doc = 0;
    std::string path;
    std::vector<Match> matches;  // empty when the query has nothing to highlight
    friend bool operator==(const FileMatch&, const FileMatch&) = default;
};

struct SearchResults {
    std::vector<FileMatch> files;  // in doc ID order
    friend bool operator==(const SearchResults&, const SearchResults&) = default;
};

// How a search ended.
enum class SearchStatus {
    Complete,   // every match was found
    Truncated,  // stopped early, by a limit
    Cancelled,  // stopped early, because the user asked
};

// Adds line and column to each span. Lines end at '\n'. `spans` must be sorted.
std::vector<Match> locate(std::string_view content, std::span<const Span> spans);

// Calls onFile for each file matching `query`, in doc ID order, with the
// file's content. The content is only valid during the call. onFile returns
// false to stop, which makes this return Truncated. If `cancelled` is set,
// it's checked before each file, and once it's true this returns Cancelled.
// The query's repo: filters must already be resolved (resolveRepo).
SearchStatus forEachMatch(
    const IndexReader& index, const QueryNode& query, const SearchOptions& options,
    const std::function<bool(const FileMatch& file, std::string_view content)>& onFile,
    const std::atomic<bool>* cancelled = nullptr);

// Every file matching `query`. For tests and small searches.
SearchResults search(const IndexReader& index, const QueryNode& query,
                     const SearchOptions& options = {});

}  // namespace codesearch
