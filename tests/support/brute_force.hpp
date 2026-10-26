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

}  // namespace codesearch::testing
