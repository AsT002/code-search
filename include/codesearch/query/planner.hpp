#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "codesearch/query/ast.hpp"
#include "codesearch/query/trigram_query.hpp"

namespace codesearch {

// Removes the parts whose answer is already known: true and false inside
// and, or and -, and an and or or with one child. An or with a true child
// keeps it, so its other children can still be highlighted. Returns false
// (a ConstNode) if no file can match, and true if every file matches with
// nothing to highlight.
QueryNode simplify(const QueryNode& query);

// The query as it applies to the repo called `repoName`: every repo:X
// becomes true if X is `repoName` and false if not, then it's simplified.
// If the result is false, nothing in this repo can match.
QueryNode resolveRepo(const QueryNode& query, std::string_view repoName);

// The names used in repo: filters, sorted, each once.
std::vector<std::string> repoNamesIn(const QueryNode& query);

// Builds a TrigramQuery that finds at least every file matching `query`. It
// may also find files that don't match; the evaluator removes those.
TrigramQuery planCandidates(const QueryNode& query);

}  // namespace codesearch
