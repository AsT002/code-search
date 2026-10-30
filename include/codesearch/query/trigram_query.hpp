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

// Docs that match at least one child.
struct OrQuery {
    std::vector<TrigramQuery> children;
    friend bool operator==(const OrQuery&, const OrQuery&) = default;
};

struct TrigramQuery {
    std::variant<AllQuery, NoneQuery, TrigramTerm, AndQuery, OrQuery> value;
    friend bool operator==(const TrigramQuery&, const TrigramQuery&) = default;
};

}  // namespace codesearch
