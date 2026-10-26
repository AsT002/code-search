#pragma once

#include <variant>
#include <vector>

#include "codesearch/index/trigram.hpp"

namespace codesearch {

struct TrigramQuery;

// Every doc.
struct AllQuery {
    friend bool operator==(const AllQuery&, const AllQuery&) = default;
};

// No doc.
struct NoneQuery {
    friend bool operator==(const NoneQuery&, const NoneQuery&) = default;
};

// Docs that contain this trigram.
struct TrigramTerm {
    Trigram trigram;
    friend bool operator==(const TrigramTerm&, const TrigramTerm&) = default;
};

// Docs that match every child.
struct AndQuery {
    std::vector<TrigramQuery> children;
    friend bool operator==(const AndQuery&, const AndQuery&) = default;
};

struct TrigramQuery {
    std::variant<AllQuery, NoneQuery, TrigramTerm, AndQuery> value;
    friend bool operator==(const TrigramQuery&, const TrigramQuery&) = default;
};

}  // namespace codesearch
