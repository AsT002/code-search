#include "codesearch/search/evaluator.hpp"

#include <algorithm>
#include <stdexcept>

#include "codesearch/query/language.hpp"

namespace codesearch {

namespace {

// Text matches if it's empty or found. An empty literal matches every file.
bool foundOrEmpty(const LiteralMatcher& matcher, std::string_view text) {
    return matcher.emptyPattern() || matcher.foundIn(text);
}

}  // namespace

Evaluator::Evaluator(const QueryNode& query, const SearchOptions& options)
    : root_(compile(query, options)) {}

Evaluator::Node Evaluator::compile(const QueryNode& query, const SearchOptions& options) {
    Node node;
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        node.kind = Kind::Literal;
        node.matcher = LiteralMatcher(lit->text, options.caseSensitive);
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
            node.children.push_back(compile(child, options));
        }
    } else if (const auto* orNode = std::get_if<OrNode>(&query.value)) {
        node.kind = Kind::Or;
        for (const QueryNode& child : orNode->children) {
            node.children.push_back(compile(child, options));
        }
    } else if (const auto* negated = std::get_if<NotNode>(&query.value)) {
        node.kind = Kind::Not;
        node.children.push_back(compile(negated->child[0], options));
    } else {
        throw std::logic_error("repo: filters must be resolved before searching");
    }
    return node;
}

bool Evaluator::matches(std::string_view path, std::string_view content) const {
    return nodeMatches(root_, path, content);
}

bool Evaluator::nodeMatches(const Node& node, std::string_view path, std::string_view content) {
    switch (node.kind) {
        case Kind::Literal:
            return foundOrEmpty(node.matcher, content);
        case Kind::Path:
            return foundOrEmpty(node.matcher, path);
        case Kind::Lang:
            return languageOf(path) == node.lang;
        case Kind::Const:
            return node.value;
        case Kind::And:
            // all_of stops at the first child that doesn't match.
            return std::all_of(node.children.begin(), node.children.end(), [&](const Node& child) {
                return nodeMatches(child, path, content);
            });
        case Kind::Or:
            // any_of stops at the first child that matches.
            return std::any_of(node.children.begin(), node.children.end(), [&](const Node& child) {
                return nodeMatches(child, path, content);
            });
        case Kind::Not:
            return !nodeMatches(node.children[0], path, content);
    }
    return false;  // every kind returned above; this keeps the compiler happy
}

std::vector<Span> Evaluator::collectSpans(std::string_view path, std::string_view content) const {
    std::vector<Span> spans;
    collect(root_, path, content, spans);
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
    return spans;
}

// Only called for a node that matches, so every child of an and matches too.
void Evaluator::collect(const Node& node, std::string_view path, std::string_view content,
                        std::vector<Span>& out) {
    switch (node.kind) {
        case Kind::Literal:
            node.matcher.findAll(content, [&out](Span span) {
                out.push_back(span);
                return true;  // keep going
            });
            return;
        case Kind::And:
            for (const Node& child : node.children) {
                collect(child, path, content, out);
            }
            return;
        case Kind::Or:
            for (const Node& child : node.children) {
                if (nodeMatches(child, path, content)) {
                    collect(child, path, content, out);
                }
            }
            return;
        case Kind::Path:
        case Kind::Lang:
        case Kind::Const:
        case Kind::Not:
            return;  // nothing in the content to highlight
    }
}

}  // namespace codesearch
