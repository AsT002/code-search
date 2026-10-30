#pragma once

#include <string>
#include <vector>

#include "codesearch/query/ast.hpp"
#include "codesearch/search/searcher.hpp"

namespace codesearch::testing {

struct TestFile {
    std::string path;
    std::string content;
};

// The slow, obvious version of search(): checks every file with plain loops.
// It only shares data types with the real search, never logic, so a bug in
// the planner, matcher or evaluator can't hide itself here.
SearchResults bruteForceSearch(const std::vector<TestFile>& files, const QueryNode& query,
                               const SearchOptions& options);

struct TestRepo {
    std::string name;
    std::vector<TestFile> files;
};

// A matching line: its number, and the matches as byte ranges inside the
// line's text (without the '\n' and a CR before it).
struct BruteLine {
    std::size_t number = 0;
    std::vector<Span> matches;
    friend bool operator==(const BruteLine&, const BruteLine&) = default;
};

struct BruteFile {
    std::string repo;
    std::string path;
    std::vector<BruteLine> lines;  // empty if there's nothing to highlight
    friend bool operator==(const BruteFile&, const BruteFile&) = default;
};

// The slow version of searchRepos, for any query: every repo, every file,
// in order. lang: only knows .go, .py and Makefile, the names tests use.
std::vector<BruteFile> bruteForceRepoSearch(const std::vector<TestRepo>& repos,
                                            const QueryNode& query, const SearchOptions& options);

}  // namespace codesearch::testing
