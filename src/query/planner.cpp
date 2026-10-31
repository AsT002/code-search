#include "codesearch/query/planner.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "codesearch/index/trigram.hpp"

namespace codesearch {

namespace {

// The value of `node` if it's a ConstNode, or nothing.
std::optional<bool> constValue(const QueryNode& node) {
    if (const auto* known = std::get_if<ConstNode>(&node.value)) {
        return known->value;
    }
    return std::nullopt;
}

// True if `node` is the constant `value`.
bool isConst(const QueryNode& node, bool value) {
    const auto* known = std::get_if<ConstNode>(&node.value);
    return known != nullptr && known->value == value;
}

// True if a simplified `node` matches every file. Besides true itself, an
// or with a true child does: simplify keeps that true next to the other
// children, so they can still be highlighted. So does an and of such parts.
bool alwaysTrue(const QueryNode& node) {
    if (const auto* orNode = std::get_if<OrNode>(&node.value)) {
        return std::any_of(orNode->children.begin(), orNode->children.end(), alwaysTrue);
    }
    if (const auto* andNode = std::get_if<AndNode>(&node.value)) {
        return std::all_of(andNode->children.begin(), andNode->children.end(), alwaysTrue);
    }
    return isConst(node, true);
}

// The children of `node` if it's an and (isAnd) or an or (!isAnd), or null.
const std::vector<QueryNode>* sameKindChildren(const QueryNode& node, bool isAnd) {
    const auto* andNode = std::get_if<AndNode>(&node.value);
    if (isAnd && andNode != nullptr) {
        return &andNode->children;
    }
    const auto* orNode = std::get_if<OrNode>(&node.value);
    if (!isAnd && orNode != nullptr) {
        return &orNode->children;
    }
    return nullptr;
}

// Simplifies an and (isAnd) or an or.
//
// In an and, true changes nothing and false makes the whole and false.
// In an or, false changes nothing. True makes the or match every file, but
// it can't replace the whole or: the other children still say what to
// highlight. So it stays, once, at the end, next to them.
QueryNode simplifyGroup(const std::vector<QueryNode>& children, bool isAnd) {
    std::vector<QueryNode> kept;
    bool hasTrue = false;  // only used for or

    // Adds one simplified child, unless its value is already known.
    const auto keep = [&](QueryNode child) {
        const std::optional<bool> known = constValue(child);
        if (!known.has_value()) {
            kept.push_back(std::move(child));
        } else if (!isAnd && known.value()) {
            hasTrue = true;
        }
    };

    for (const QueryNode& child : children) {
        QueryNode simple = simplify(child);
        const std::optional<bool> known = constValue(simple);
        if (isAnd && known.has_value() && !known.value()) {
            return constant(false);
        }
        // An and inside an and (or an or inside an or): lift its children up.
        const std::vector<QueryNode>* inner = sameKindChildren(simple, isAnd);
        if (inner != nullptr) {
            for (const QueryNode& grandchild : *inner) {
                keep(grandchild);
            }
        } else {
            keep(std::move(simple));
        }
    }

    if (hasTrue) {
        if (kept.empty()) {
            return constant(true);
        }
        kept.push_back(constant(true));
    }
    if (kept.empty()) {
        return constant(isAnd);  // an empty and is true, an empty or is false
    }
    if (kept.size() == 1) {
        return std::move(kept[0]);
    }
    return isAnd ? allOf(std::move(kept)) : anyOf(std::move(kept));
}

QueryNode replaceRepos(const QueryNode& query, std::string_view repoName) {
    if (const auto* repo = std::get_if<RepoNode>(&query.value)) {
        return constant(repo->name == repoName);
    }
    if (const auto* node = std::get_if<AndNode>(&query.value)) {
        std::vector<QueryNode> children;
        children.reserve(node->children.size());
        for (const QueryNode& child : node->children) {
            children.push_back(replaceRepos(child, repoName));
        }
        return allOf(std::move(children));
    }
    if (const auto* node = std::get_if<OrNode>(&query.value)) {
        std::vector<QueryNode> children;
        children.reserve(node->children.size());
        for (const QueryNode& child : node->children) {
            children.push_back(replaceRepos(child, repoName));
        }
        return anyOf(std::move(children));
    }
    if (const auto* node = std::get_if<NotNode>(&query.value)) {
        return negate(replaceRepos(node->child[0], repoName));
    }
    return query;  // text and the other filters stay as they are
}

void collectRepoNames(const QueryNode& query, std::vector<std::string>& out) {
    if (const auto* repo = std::get_if<RepoNode>(&query.value)) {
        out.push_back(repo->name);
    } else if (const auto* andNode = std::get_if<AndNode>(&query.value)) {
        for (const QueryNode& child : andNode->children) {
            collectRepoNames(child, out);
        }
    } else if (const auto* orNode = std::get_if<OrNode>(&query.value)) {
        for (const QueryNode& child : orNode->children) {
            collectRepoNames(child, out);
        }
    } else if (const auto* notNode = std::get_if<NotNode>(&query.value)) {
        collectRepoNames(notNode->child[0], out);
    }
}

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

TrigramQuery planOr(const OrNode& node) {
    OrQuery query;
    for (const QueryNode& child : node.children) {
        TrigramQuery planned = planCandidates(child);

        if (std::holds_alternative<AllQuery>(planned.value)) {
            return planned;  // one child could match any file, so all of them can
        }
        if (std::holds_alternative<NoneQuery>(planned.value)) {
            continue;  // adds no files
        }
        if (auto* nested = std::get_if<OrQuery>(&planned.value)) {
            for (TrigramQuery& grandchild : nested->children) {
                query.children.push_back(std::move(grandchild));
            }
            continue;
        }
        query.children.push_back(std::move(planned));
    }

    if (query.children.empty()) {
        return {NoneQuery{}};
    }
    if (query.children.size() == 1) {
        return query.children[0];
    }
    return {query};
}

}  // namespace

QueryNode simplify(const QueryNode& query) {
    if (const auto* node = std::get_if<AndNode>(&query.value)) {
        return simplifyGroup(node->children, true);
    }
    if (const auto* node = std::get_if<OrNode>(&query.value)) {
        return simplifyGroup(node->children, false);
    }
    if (const auto* node = std::get_if<NotNode>(&query.value)) {
        QueryNode child = simplify(node->child[0]);
        // Nothing under a '-' is highlighted, so a child that matches every
        // file can turn into plain false here.
        if (alwaysTrue(child)) {
            return constant(false);
        }
        if (isConst(child, false)) {
            return constant(true);
        }
        return negate(std::move(child));
    }
    return query;
}

QueryNode resolveRepo(const QueryNode& query, std::string_view repoName) {
    return simplify(replaceRepos(query, repoName));
}

std::vector<std::string> repoNamesIn(const QueryNode& query) {
    std::vector<std::string> names;
    collectRepoNames(query, names);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

TrigramQuery planCandidates(const QueryNode& query) {
    if (const auto* node = std::get_if<LiteralNode>(&query.value)) {
        return planLiteral(*node);
    }
    if (const auto* node = std::get_if<AndNode>(&query.value)) {
        return planAnd(*node);
    }
    if (const auto* node = std::get_if<OrNode>(&query.value)) {
        return planOr(*node);
    }
    if (const auto* node = std::get_if<ConstNode>(&query.value)) {
        return node->value ? TrigramQuery{AllQuery{}} : TrigramQuery{NoneQuery{}};
    }
    // Filters and '-': the index can't narrow these down, so every file is a
    // candidate and the evaluator decides. Taking the opposite of a candidate
    // list for '-' would be wrong: its extra files would become missed matches.
    return {AllQuery{}};
}

}  // namespace codesearch
