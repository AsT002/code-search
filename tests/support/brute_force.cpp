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

bool contains(std::string_view text, std::string_view pattern, bool caseSensitive) {
    return pattern.empty() || !occurrences(text, pattern, caseSensitive).empty();
}

std::string_view languageOfTestPath(std::string_view path) {
    if (path.ends_with(".go")) {
        return "go";
    }
    if (path.ends_with(".py")) {
        return "python";
    }
    if (path == "Makefile" || path.ends_with("/Makefile")) {
        return "make";
    }
    return "";
}

// Everything the query can look at for one file.
struct FileView {
    std::string_view repo;
    std::string_view path;
    std::string_view content;
    bool caseSensitive = false;
};

bool fileMatches(const FileView& file, const QueryNode& query) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        return contains(file.content, lit->text, file.caseSensitive);
    }
    if (const auto* path = std::get_if<PathNode>(&query.value)) {
        return contains(file.path, path->text, file.caseSensitive);
    }
    if (const auto* lang = std::get_if<LangNode>(&query.value)) {
        return languageOfTestPath(file.path) == lang->name;
    }
    if (const auto* repo = std::get_if<RepoNode>(&query.value)) {
        return repo->name == file.repo;
    }
    if (const auto* known = std::get_if<ConstNode>(&query.value)) {
        return known->value;
    }
    if (const auto* andNode = std::get_if<AndNode>(&query.value)) {
        for (const QueryNode& child : andNode->children) {
            if (!fileMatches(file, child)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* orNode = std::get_if<OrNode>(&query.value)) {
        for (const QueryNode& child : orNode->children) {
            if (fileMatches(file, child)) {
                return true;
            }
        }
        return false;
    }
    return !fileMatches(file, std::get<NotNode>(query.value).child[0]);
}

// The text to highlight, from the parts of the query that matched.
void allSpans(const FileView& file, const QueryNode& query, std::vector<Span>& out) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        for (const Span span : occurrences(file.content, lit->text, file.caseSensitive)) {
            out.push_back(span);
        }
    } else if (const auto* andNode = std::get_if<AndNode>(&query.value)) {
        for (const QueryNode& child : andNode->children) {
            allSpans(file, child, out);
        }
    } else if (const auto* orNode = std::get_if<OrNode>(&query.value)) {
        for (const QueryNode& child : orNode->children) {
            if (fileMatches(file, child)) {
                allSpans(file, child, out);
            }
        }
    }
}

std::vector<Span> sortedSpans(const FileView& file, const QueryNode& query) {
    std::vector<Span> spans;
    allSpans(file, query, spans);
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
    return spans;
}

// True if text[i] is a '\n' or a '\r' right before one.
bool isLineEndingByte(std::string_view text, std::size_t i) {
    if (text[i] == '\n') {
        return true;
    }
    return text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n';
}

// Line and column of a match, by counting newlines from the start each time.
// A match that starts in a line ending begins at its first other byte; one
// made only of line endings stays where it is.
Match position(std::string_view text, Span span) {
    std::size_t start = span.begin;
    while (start < span.end && isLineEndingByte(text, start)) {
        ++start;
    }
    if (start == span.end) {
        start = span.begin;
    }
    std::size_t line = 1;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < start; ++i) {
        if (text[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    Match match;
    match.bytes = Span{start, span.end};
    match.line = line;
    match.column = start - lineStart + 1;
    return match;
}

// Every span's position, in order of where they start.
std::vector<Match> positions(std::string_view text, const std::vector<Span>& spans) {
    std::vector<Match> matches;
    matches.reserve(spans.size());
    for (const Span span : spans) {
        matches.push_back(position(text, span));
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        return a.bytes.begin < b.bytes.begin;
    });
    return matches;
}

// A match as a line number and a range inside that line's text.
BruteLine lineOf(std::string_view text, const Match& match) {
    const Span span = match.bytes;
    const std::size_t lineStart = span.begin - (match.column - 1);
    std::size_t lineEnd = lineStart;
    while (lineEnd < text.size() && text[lineEnd] != '\n') {
        ++lineEnd;
    }
    const bool endsWithNewline = lineEnd < text.size();
    if (endsWithNewline && lineEnd > lineStart && text[lineEnd - 1] == '\r') {
        --lineEnd;
    }
    const std::size_t begin = std::min(span.begin, lineEnd) - lineStart;
    const std::size_t end = std::min(span.end, lineEnd) - lineStart;
    return BruteLine{.number = match.line, .matches = {Span{begin, end}}};
}

}  // namespace

SearchResults bruteForceSearch(const std::vector<TestFile>& files, const QueryNode& query,
                               const SearchOptions& options) {
    SearchResults results;
    for (std::size_t id = 0; id < files.size(); ++id) {
        const TestFile& file = files[id];
        const FileView view{
            .repo = "",
            .path = file.path,
            .content = file.content,
            .caseSensitive = options.caseSensitive,
        };
        if (!fileMatches(view, query)) {
            continue;
        }
        FileMatch match;
        match.doc = static_cast<DocId>(id);
        match.path = file.path;
        match.matches = positions(file.content, sortedSpans(view, query));
        results.files.push_back(match);
    }
    return results;
}

std::vector<BruteFile> bruteForceRepoSearch(const std::vector<TestRepo>& repos,
                                            const QueryNode& query, const SearchOptions& options) {
    std::vector<BruteFile> results;
    for (const TestRepo& repo : repos) {
        for (const TestFile& file : repo.files) {
            const FileView view{
                .repo = repo.name,
                .path = file.path,
                .content = file.content,
                .caseSensitive = options.caseSensitive,
            };
            if (!fileMatches(view, query)) {
                continue;
            }
            BruteFile found{.repo = repo.name, .path = file.path, .lines = {}};
            for (const Match& at : positions(file.content, sortedSpans(view, query))) {
                BruteLine line = lineOf(file.content, at);
                if (!found.lines.empty() && found.lines.back().number == line.number) {
                    found.lines.back().matches.push_back(line.matches[0]);
                } else {
                    found.lines.push_back(std::move(line));
                }
            }
            results.push_back(std::move(found));
        }
    }
    return results;
}

}  // namespace codesearch::testing
