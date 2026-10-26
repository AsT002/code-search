#include "codesearch/search/evaluator.hpp"

#include <algorithm>

namespace codesearch {

Evaluator::Evaluator(const QueryNode& query, const SearchOptions& options)
    : root_(compile(query, options)) {}

Evaluator::Node Evaluator::compile(const QueryNode& query, const SearchOptions& options) {
    Node node;
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        node.literal = LiteralMatcher(lit->text, options.caseSensitive);
        return node;
    }
    for (const QueryNode& child : std::get<AndNode>(query.value).children) {
        node.children.push_back(compile(child, options));
    }
    return node;
}

bool Evaluator::matches(std::string_view content) const {
    return nodeMatches(root_, content);
}

bool Evaluator::nodeMatches(const Node& node, std::string_view content) {
    if (node.literal.has_value()) {
        // An empty literal matches every file.
        return node.literal->emptyPattern() || node.literal->foundIn(content);
    }
    for (const Node& child : node.children) {
        if (!nodeMatches(child, content)) {
            return false;
        }
    }
    return true;
}

std::vector<Span> Evaluator::collectSpans(std::string_view content) const {
    std::vector<Span> spans;
    collect(root_, content, spans);
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
    return spans;
}

void Evaluator::collect(const Node& node, std::string_view content, std::vector<Span>& out) {
    if (node.literal.has_value()) {
        node.literal->findAll(content, [&out](Span span) {
            out.push_back(span);
            return true;  // keep going
        });
        return;
    }
    for (const Node& child : node.children) {
        collect(child, content, out);
    }
}

}  // namespace codesearch
