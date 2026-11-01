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

// /pattern/: an RE2 regular expression. The file's content has a match.
// An empty match counts, but only non-empty matches are highlighted.
struct RegexNode {
    std::string pattern;
    friend bool operator==(const RegexNode&, const RegexNode&) = default;
};

// path:/pattern/ and file:/pattern/. The file's path has a match.
struct PathRegexNode {
    std::string pattern;
    friend bool operator==(const PathRegexNode&, const PathRegexNode&) = default;
};

// path:text and file:text. The file's path contains `text`.
struct PathNode {
    std::string text;
    friend bool operator==(const PathNode&, const PathNode&) = default;
};

// lang:name. The file's language, worked out from its name (languageOf), is
// `name`. `name` is the language's main name, like "cpp" for lang:c++.
struct LangNode {
    std::string name;
    friend bool operator==(const LangNode&, const LangNode&) = default;
};

// repo:name. The file is in the repo with exactly this name. The planner
// swaps this for a ConstNode before searching each repo (resolveRepo).
struct RepoNode {
    std::string name;
    friend bool operator==(const RepoNode&, const RepoNode&) = default;
};

// Always true or always false. Only the planner makes these.
struct ConstNode {
    bool value = true;
    friend bool operator==(const ConstNode&, const ConstNode&) = default;
};

// Every child must match somewhere in the same file.
struct AndNode {
    std::vector<QueryNode> children;
    friend bool operator==(const AndNode&, const AndNode&) = default;
};

// At least one child must match.
struct OrNode {
    std::vector<QueryNode> children;
    friend bool operator==(const OrNode&, const OrNode&) = default;
};

// The child must not match. A vector holding exactly one child: QueryNode
// isn't fully defined yet at this point, and a vector is allowed to hold a
// type like that, while a plain member isn't.
struct NotNode {
    std::vector<QueryNode> child;
    friend bool operator==(const NotNode&, const NotNode&) = default;
};

struct QueryNode {
    std::variant<LiteralNode, RegexNode, PathNode, PathRegexNode, LangNode, RepoNode, ConstNode,
                 AndNode, OrNode, NotNode>
        value;
    friend bool operator==(const QueryNode&, const QueryNode&) = default;
};

inline QueryNode literal(std::string text) {
    return {LiteralNode{std::move(text)}};
}

inline QueryNode regex(std::string pattern) {
    return {RegexNode{std::move(pattern)}};
}

inline QueryNode pathRegex(std::string pattern) {
    return {PathRegexNode{std::move(pattern)}};
}

inline QueryNode pathFilter(std::string text) {
    return {PathNode{std::move(text)}};
}

inline QueryNode langFilter(std::string name) {
    return {LangNode{std::move(name)}};
}

inline QueryNode repoFilter(std::string name) {
    return {RepoNode{std::move(name)}};
}

inline QueryNode constant(bool value) {
    return {ConstNode{value}};
}

inline QueryNode allOf(std::vector<QueryNode> children) {
    return {AndNode{std::move(children)}};
}

inline QueryNode anyOf(std::vector<QueryNode> children) {
    return {OrNode{std::move(children)}};
}

inline QueryNode negate(QueryNode child) {
    return {NotNode{{std::move(child)}}};
}

struct SearchOptions {
    bool caseSensitive = false;  // for the text, regexes and path:
};

}  // namespace codesearch
