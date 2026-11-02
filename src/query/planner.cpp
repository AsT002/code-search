#include "codesearch/query/planner.hpp"

#include <re2/prefilter.h>
#include <re2/re2.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
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

// The candidates of every part at once: parts that allow every file are
// left out, one that allows none makes it none, and an and inside an and is
// lifted up a level.
TrigramQuery combineAnd(std::vector<TrigramQuery> parts) {
    AndQuery query;
    for (TrigramQuery& planned : parts) {
        if (std::holds_alternative<NoneQuery>(planned.value)) {
            return planned;  // one part can't match, so nothing can
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

// The candidates of any of the parts: one part that allows every file makes
// it every file, ones that allow none are left out.
TrigramQuery combineOr(std::vector<TrigramQuery> parts) {
    OrQuery query;
    for (TrigramQuery& planned : parts) {
        if (std::holds_alternative<AllQuery>(planned.value)) {
            return planned;  // one part could match any file, so all of them can
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

TrigramQuery plan(const QueryNode& query, const SearchOptions& options);

std::vector<TrigramQuery> planEach(const std::vector<QueryNode>& children,
                                   const SearchOptions& options) {
    std::vector<TrigramQuery> parts;
    parts.reserve(children.size());
    for (const QueryNode& child : children) {
        parts.push_back(plan(child, options));
    }
    return parts;
}

// True if `pattern` could name a character outside ASCII: with its bytes,
// \x{...}, or a Unicode class like \p{L}. RE2's prefilter lowercases those
// with Unicode rules, and some become ASCII (U+212A KELVIN SIGN becomes k),
// which the index, folding only ASCII, wouldn't find.
bool mayNameNonAscii(std::string_view pattern) {
    const bool nonAsciiByte = std::any_of(pattern.begin(), pattern.end(), [](char c) {
        return static_cast<unsigned char>(c) >= 0x80;
    });
    // An octal escape like \577 (LONG S) too: a backslash and a digit.
    bool octal = false;
    for (std::size_t i = 0; i + 1 < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            octal = octal || (pattern[i + 1] >= '0' && pattern[i + 1] <= '9');
            ++i;  // skip what the backslash escapes
        }
    }
    return nonAsciiByte || octal || pattern.find("\\x{") != std::string_view::npos ||
           pattern.find("\\p") != std::string_view::npos ||
           pattern.find("\\P") != std::string_view::npos;
}

// True if `pattern` turns case folding on itself, with a flag like (?i) or
// (?si:...). Then RE2 folds case even under case:yes. Any 'i' among the
// flags counts, even after a '-' that turns it off: that only makes the
// plan check more files.
bool hasCaseFlag(std::string_view pattern) {
    for (std::size_t at = pattern.find("(?"); at != std::string_view::npos;
         at = pattern.find("(?", at + 2)) {
        for (std::size_t i = at + 2; i < pattern.size(); ++i) {
            const char c = pattern[i];
            if (c == 'i') {
                return true;
            }
            const bool flag = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
            if (!flag) {
                break;  // the end of the flags
            }
        }
    }
    return false;
}

// Turns RE2's prefilter tree into candidates. Its atoms are pieces of text,
// lowercased, that a match needs, joined by AND and OR. An atom is only
// used if the index finds it the same way. Without case, an atom with a 'k'
// isn't: RE2 then also matches KELVIN SIGN for it. Nor is one with bytes
// outside ASCII. (After mayNameNonAscii, those only come from case variants
// like LONG S for s, which the index does keep as they are; this is a second
// guard, in case RE2 lowercases more in a later version.) Others allow every
// file.
TrigramQuery fromPrefilter(re2::Prefilter& node, bool caseSensitive) {
    switch (node.op()) {
        case re2::Prefilter::ALL:
            return {AllQuery{}};
        case re2::Prefilter::NONE:
            return {NoneQuery{}};
        case re2::Prefilter::ATOM: {
            const std::string& atom = node.atom();
            const bool ascii = std::all_of(atom.begin(), atom.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x80;
            });
            if (!ascii) {
                return {AllQuery{}};
            }
            if (caseSensitive) {
                return planLiteral(LiteralNode{atom});
            }
            // RE2 folds k with KELVIN SIGN and s with LONG S, the only ASCII
            // letters it folds with something outside ASCII. A match may
            // have those in their place, so only the text between them is
            // sure to be there: "mustcompile" needs "mu" and "tcompile".
            std::vector<TrigramQuery> pieces;
            std::size_t start = 0;
            while (true) {
                const std::size_t stop = atom.find_first_of("ks", start);
                pieces.push_back(planLiteral(LiteralNode{atom.substr(start, stop - start)}));
                if (stop == std::string::npos) {
                    break;
                }
                start = stop + 1;
            }
            return combineAnd(std::move(pieces));
        }
        case re2::Prefilter::AND:
        case re2::Prefilter::OR: {
            std::vector<TrigramQuery> parts;
            for (re2::Prefilter* sub : *node.subs()) {
                parts.push_back(fromPrefilter(*sub, caseSensitive));
            }
            return node.op() == re2::Prefilter::AND ? combineAnd(std::move(parts))
                                                    : combineOr(std::move(parts));
        }
    }
    return {AllQuery{}};  // every op returned above; this keeps the compiler happy
}

// The candidates for a regex, from the same AND/OR tree of required text
// that RE2's FilteredRE2 uses. /foo|bar/ becomes foo OR bar.
TrigramQuery planRegex(const RegexNode& node, bool caseSensitive) {
    if (mayNameNonAscii(node.pattern)) {
        return {AllQuery{}};
    }
    // Compiled the way RegexMatcher compiles it, so the tree fits what's run.
    RE2::Options re2Options;
    re2Options.set_case_sensitive(caseSensitive);
    re2Options.set_never_nl(true);
    re2Options.set_log_errors(false);
    const RE2 compiled("(?m)" + node.pattern, re2Options);
    // A flag in the pattern can fold case even under case:yes.
    caseSensitive = caseSensitive && !hasCaseFlag(node.pattern);
    if (!compiled.ok()) {
        return {AllQuery{}};
    }
    // FromRE2 returns a new tree, or null; unique_ptr deletes it.
    const std::unique_ptr<re2::Prefilter> tree(re2::Prefilter::FromRE2(&compiled));
    if (tree == nullptr) {
        return {AllQuery{}};
    }
    return fromPrefilter(*tree, caseSensitive);
}

TrigramQuery plan(const QueryNode& query, const SearchOptions& options) {
    if (const auto* node = std::get_if<LiteralNode>(&query.value)) {
        return planLiteral(*node);
    }
    if (const auto* node = std::get_if<RegexNode>(&query.value)) {
        return planRegex(*node, options.caseSensitive);
    }
    if (const auto* node = std::get_if<AndNode>(&query.value)) {
        return combineAnd(planEach(node->children, options));
    }
    if (const auto* node = std::get_if<OrNode>(&query.value)) {
        return combineOr(planEach(node->children, options));
    }
    if (const auto* node = std::get_if<ConstNode>(&query.value)) {
        return node->value ? TrigramQuery{AllQuery{}} : TrigramQuery{NoneQuery{}};
    }
    // Filters and '-': the index can't narrow these down, so every file is a
    // candidate and the evaluator decides. Taking the opposite of a candidate
    // list for '-' would be wrong: its extra files would become missed matches.
    return {AllQuery{}};
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

TrigramQuery planCandidates(const QueryNode& query, const SearchOptions& options) {
    return plan(query, options);
}

}  // namespace codesearch
