#include "codesearch/search/evaluator.hpp"

#include <algorithm>
#include <stdexcept>

#include "codesearch/query/language.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"

namespace codesearch {

namespace {

// Text matches if it's empty or found. An empty literal matches every file.
bool foundOrEmpty(const LiteralMatcher& matcher, std::string_view text) {
    return matcher.emptyPattern() || matcher.foundIn(text);
}

// The value in `value`, which the node's kind says is set (a Regex node
// always has its regex).
template <typename T>
const T& set(const std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::logic_error("a query node is missing its matcher");
    }
    return *value;
}

}  // namespace

Evaluator::Evaluator(const QueryNode& query, const SearchOptions& options)
    : root_(compile(query, options, true)) {}

// `required` is true while every node above this one is an and: then a
// matching file must contain each literal here, so every candidate already
// has its trigrams.
Evaluator::Node Evaluator::compile(const QueryNode& query, const SearchOptions& options,
                                   bool required) {
    Node node;
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        node.kind = Kind::Literal;
        node.matcher = LiteralMatcher(lit->text, options.caseSensitive);
        node.text = lit->text;
        node.required = required;
    } else if (const auto* pattern = std::get_if<RegexNode>(&query.value)) {
        node.kind = Kind::Regex;
        node.regex.emplace(pattern->pattern, options.caseSensitive);
    } else if (const auto* pathPattern = std::get_if<PathRegexNode>(&query.value)) {
        node.kind = Kind::PathRegex;
        node.regex.emplace(pathPattern->pattern, options.caseSensitive);
    } else if (const auto* path = std::get_if<PathNode>(&query.value)) {
        node.kind = Kind::Path;
        node.matcher = LiteralMatcher(path->text, options.caseSensitive);
    } else if (const auto* lang = std::get_if<LangNode>(&query.value)) {
        node.kind = Kind::Lang;
        node.lang = lang->name;
    } else if (const auto* known = std::get_if<ConstNode>(&query.value)) {
        node.kind = Kind::Const;
        node.value = known->value;
    } else if (const auto* andNode = std::get_if<AndNode>(&query.value)) {
        node.kind = Kind::And;
        for (const QueryNode& child : andNode->children) {
            node.children.push_back(compile(child, options, required));
        }
    } else if (const auto* orNode = std::get_if<OrNode>(&query.value)) {
        node.kind = Kind::Or;
        for (const QueryNode& child : orNode->children) {
            node.children.push_back(compile(child, options, false));
        }
    } else if (const auto* negated = std::get_if<NotNode>(&query.value)) {
        node.kind = Kind::Not;
        node.children.push_back(compile(negated->child[0], options, false));
    } else {
        throw std::logic_error("repo: filters must be resolved before searching");
    }
    return node;
}

void Evaluator::useIndex(const IndexReader& index) {
    lookUp(root_, index);
}

void Evaluator::lookUp(Node& node, const IndexReader& index) {
    for (Node& child : node.children) {
        lookUp(child, index);
    }
    if (node.kind != Kind::Literal || node.required) {
        return;
    }
    const TrigramQuery plan = planCandidates(literal(node.text));
    if (std::holds_alternative<AllQuery>(plan.value)) {
        return;  // too short to have a trigram; every file might contain it
    }
    node.mightContain = findCandidates(index, plan);
    node.usesIndex = true;
}

bool Evaluator::usesRegex() const {
    return hasRegex(root_);
}

bool Evaluator::hasRegex(const Node& node) {
    if (node.regex.has_value()) {
        return true;
    }
    for (const Node& child : node.children) {
        if (hasRegex(child)) {
            return true;
        }
    }
    return false;
}

Evaluator Evaluator::withOwnRegexes() const {
    Evaluator copy = *this;
    ownRegexes(copy.root_);
    return copy;
}

void Evaluator::ownRegexes(Node& node) {
    if (node.regex.has_value()) {
        node.regex = node.regex->withOwnCopy();
    }
    for (Node& child : node.children) {
        ownRegexes(child);
    }
}

bool Evaluator::matches(std::string_view path, std::string_view content) const {
    return nodeMatches(root_, File{.doc = std::nullopt, .path = path, .content = content});
}

bool Evaluator::matches(DocId doc, std::string_view path, std::string_view content) const {
    return nodeMatches(root_, File{.doc = doc, .path = path, .content = content});
}

bool Evaluator::nodeMatches(const Node& node, const File& file) {
    switch (node.kind) {
        case Kind::Literal:
            // A file without the literal's trigrams can't contain it.
            if (node.usesIndex && file.doc.has_value() &&
                !std::binary_search(node.mightContain.begin(), node.mightContain.end(),
                                    file.doc.value())) {
                return false;
            }
            return foundOrEmpty(node.matcher, file.content);
        case Kind::Regex:
            return set(node.regex).foundIn(file.content);
        case Kind::Path:
            return foundOrEmpty(node.matcher, file.path);
        case Kind::PathRegex:
            return set(node.regex).foundIn(file.path);
        case Kind::Lang:
            return languageOf(file.path) == node.lang;
        case Kind::Const:
            return node.value;
        case Kind::And:
            // all_of stops at the first child that doesn't match.
            return std::all_of(node.children.begin(), node.children.end(), [&](const Node& child) {
                return nodeMatches(child, file);
            });
        case Kind::Or:
            // any_of stops at the first child that matches.
            return std::any_of(node.children.begin(), node.children.end(), [&](const Node& child) {
                return nodeMatches(child, file);
            });
        case Kind::Not:
            return !nodeMatches(node.children[0], file);
    }
    return false;  // every kind returned above; this keeps the compiler happy
}

std::vector<Span> Evaluator::collectSpans(std::string_view path, std::string_view content) const {
    std::vector<Span> spans;
    collect(root_, File{.doc = std::nullopt, .path = path, .content = content}, spans);
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
    return spans;
}

std::vector<Span> Evaluator::collectSpans(DocId doc, std::string_view path,
                                          std::string_view content) const {
    std::vector<Span> spans;
    collect(root_, File{.doc = doc, .path = path, .content = content}, spans);
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
    return spans;
}

// Only called for a node that matches, so every child of an and matches too.
void Evaluator::collect(const Node& node, const File& file, std::vector<Span>& out) {
    switch (node.kind) {
        case Kind::Literal:
            node.matcher.findAll(file.content, [&out](Span span) {
                out.push_back(span);
                return true;  // keep going
            });
            return;
        case Kind::Regex:
            set(node.regex).findAll(file.content, [&out](Span span) {
                out.push_back(span);
                return true;  // keep going
            });
            return;
        case Kind::And:
            for (const Node& child : node.children) {
                collect(child, file, out);
            }
            return;
        case Kind::Or:
            for (const Node& child : node.children) {
                if (nodeMatches(child, file)) {
                    collect(child, file, out);
                }
            }
            return;
        case Kind::Path:
        case Kind::PathRegex:
        case Kind::Lang:
        case Kind::Const:
        case Kind::Not:
            return;  // nothing in the content to highlight
    }
}

}  // namespace codesearch
