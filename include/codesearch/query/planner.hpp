#pragma once

#include "codesearch/query/ast.hpp"
#include "codesearch/query/trigram_query.hpp"

namespace codesearch {

// Builds a TrigramQuery that finds at least every file matching `query`. It may
// also find files that don't match; the evaluator removes those.
TrigramQuery planCandidates(const QueryNode& query);

}  // namespace codesearch
