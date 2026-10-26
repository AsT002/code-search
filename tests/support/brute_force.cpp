#include "brute_force.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace codesearch::testing {

namespace {

char lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return c;
}

bool sameAt(std::string_view text, std::size_t at, std::string_view pattern, bool caseSensitive) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char a = text[at + i];
        char b = pattern[i];
        if (!caseSensitive) {
            a = lower(a);
            b = lower(b);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// Non-overlapping matches, left to right.
std::vector<Span> occurrences(std::string_view text, std::string_view pattern, bool caseSensitive) {
    std::vector<Span> spans;
    if (pattern.empty()) {
        return spans;
    }
    std::size_t i = 0;
    while (i + pattern.size() <= text.size()) {
        if (sameAt(text, i, pattern, caseSensitive)) {
            spans.push_back(Span{i, i + pattern.size()});
            i += pattern.size();
        } else {
            ++i;
        }
    }
    return spans;
}

bool fileMatches(std::string_view text, const QueryNode& query, bool caseSensitive) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        return lit->text.empty() || !occurrences(text, lit->text, caseSensitive).empty();
    }
    for (const QueryNode& child : std::get<AndNode>(query.value).children) {
        if (!fileMatches(text, child, caseSensitive)) {
            return false;
        }
    }
    return true;
}

void allSpans(std::string_view text, const QueryNode& query, bool caseSensitive,
              std::vector<Span>& out) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        for (const Span span : occurrences(text, lit->text, caseSensitive)) {
            out.push_back(span);
        }
        return;
    }
    for (const QueryNode& child : std::get<AndNode>(query.value).children) {
        allSpans(text, child, caseSensitive, out);
    }
}

// Line and column of a match, by counting newlines from the start each time.
Match position(std::string_view text, Span span) {
    std::size_t line = 1;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < span.begin; ++i) {
        if (text[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    Match match;
    match.bytes = span;
    match.line = line;
    match.column = span.begin - lineStart + 1;
    return match;
}

}  // namespace

SearchResults bruteForceSearch(const std::vector<TestFile>& files, const QueryNode& query,
                               const SearchOptions& options) {
    SearchResults results;
    for (std::size_t id = 0; id < files.size(); ++id) {
        const TestFile& file = files[id];
        if (!fileMatches(file.content, query, options.caseSensitive)) {
            continue;
        }

        std::vector<Span> spans;
        allSpans(file.content, query, options.caseSensitive, spans);
        std::sort(spans.begin(), spans.end());
        spans.erase(std::unique(spans.begin(), spans.end()), spans.end());

        FileMatch match;
        match.doc = static_cast<DocId>(id);
        match.path = file.path;
        for (const Span span : spans) {
            match.matches.push_back(position(file.content, span));
        }
        results.files.push_back(match);
    }
    return results;
}

}  // namespace codesearch::testing
