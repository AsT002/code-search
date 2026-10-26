#include "codesearch/query/planner.hpp"

#include <utility>

#include "codesearch/index/trigram.hpp"

namespace codesearch {

namespace {

TrigramQuery planLiteral(const LiteralNode& node) {
    // Shorter than a trigram: the index can't help, so every file is a candidate.
    if (node.text.size() < 3) {
        return {AllQuery{}};
    }
    // Trigrams are case-folded, so this also works for case-sensitive search.
    const std::vector<Trigram> trigrams = extractTrigrams(node.text);
    if (trigrams.size() == 1) {
        return {TrigramTerm{trigrams[0]}};
    }
    AndQuery query;
    for (const Trigram trigram : trigrams) {
        query.children.push_back({TrigramTerm{trigram}});
    }
    return {query};
}

TrigramQuery planAnd(const AndNode& node) {
    AndQuery query;
    for (const QueryNode& child : node.children) {
        TrigramQuery planned = planCandidates(child);

        if (std::holds_alternative<NoneQuery>(planned.value)) {
            return planned;  // one child can't match, so nothing can
        }
        if (std::holds_alternative<AllQuery>(planned.value)) {
            continue;  // doesn't narrow anything, so leave it out
        }
        if (auto* nested = std::get_if<AndQuery>(&planned.value)) {
            // AND inside AND: lift the inner children up one level.
            for (TrigramQuery& grandchild : nested->children) {
                query.children.push_back(std::move(grandchild));
            }
            continue;
        }
        query.children.push_back(std::move(planned));
    }

    if (query.children.empty()) {
        return {AllQuery{}};
    }
    if (query.children.size() == 1) {
        return query.children[0];
    }
    return {query};
}

}  // namespace

TrigramQuery planCandidates(const QueryNode& query) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        return planLiteral(*lit);
    }
    return planAnd(std::get<AndNode>(query.value));
}

}  // namespace codesearch
