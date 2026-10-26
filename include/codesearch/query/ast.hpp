#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace codesearch {

struct QueryNode;

// Text to find. An empty literal matches every file and highlights nothing.
struct LiteralNode {
    std::string text;
    friend bool operator==(const LiteralNode&, const LiteralNode&) = default;
};

// Every child must match somewhere in the same file.
struct AndNode {
    std::vector<QueryNode> children;
    friend bool operator==(const AndNode&, const AndNode&) = default;
};

struct QueryNode {
    std::variant<LiteralNode, AndNode> value;
    friend bool operator==(const QueryNode&, const QueryNode&) = default;
};

inline QueryNode literal(std::string text) {
    return {LiteralNode{std::move(text)}};
}

inline QueryNode allOf(std::vector<QueryNode> children) {
    return {AndNode{std::move(children)}};
}

struct SearchOptions {
    bool caseSensitive = false;
};

}  // namespace codesearch
