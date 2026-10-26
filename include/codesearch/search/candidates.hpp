#pragma once

#include <vector>

#include "codesearch/index/index_reader.hpp"
#include "codesearch/query/trigram_query.hpp"

namespace codesearch {

// Runs `query` against the index. Returns doc IDs in increasing order.
std::vector<DocId> findCandidates(const IndexReader& index, const TrigramQuery& query);

}  // namespace codesearch
