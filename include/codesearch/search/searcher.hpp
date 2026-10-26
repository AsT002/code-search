#pragma once

#include <cstddef>
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

// Adds line and column to each span. Lines end at '\n'. `spans` must be sorted.
std::vector<Match> locate(std::string_view content, std::span<const Span> spans);

SearchResults search(const IndexReader& index, const QueryNode& query,
                     const SearchOptions& options = {});

}  // namespace codesearch
