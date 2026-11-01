#include "codesearch/search/searcher.hpp"

#include <algorithm>

#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/evaluator.hpp"

namespace codesearch {

namespace {

// True if content[at] is part of a line ending: a '\n', or a '\r' right
// before one. Those bytes aren't shown.
bool inLineEnding(std::string_view content, std::size_t at) {
    return content[at] == '\n' ||
           (content[at] == '\r' && at + 1 < content.size() && content[at + 1] == '\n');
}

}  // namespace

std::vector<Match> locate(std::string_view content, std::span<const Span> spans) {
    std::vector<Match> matches;
    bool moved = false;  // a match was moved past a line ending

    // Walk the content once, keeping track of which line we're on.
    std::size_t line = 1;
    std::size_t lineStart = 0;
    std::size_t scanned = 0;
    for (const Span& span : spans) {
        // Count the newlines between the last span and this one in one pass:
        // std::count has no early exit, so the compiler checks many bytes per
        // step. Then the last of them is where this span's line starts.
        const std::string_view gap = content.substr(scanned, span.begin - scanned);
        const auto newlines = static_cast<std::size_t>(std::count(gap.begin(), gap.end(), '\n'));
        if (newlines > 0) {
            line += newlines;
            lineStart = scanned + gap.rfind('\n') + 1;
        }
        scanned = span.begin;
        Match match;
        match.bytes = span;
        match.line = line;
        match.column = span.begin - lineStart + 1;

        // A match that starts in a line ending, like "\nfoo", is shown from
        // its first visible byte, on the next line. One that's all line
        // endings has nothing to show, so it stays where it starts.
        std::size_t visible = span.begin;
        std::size_t visibleLine = line;
        std::size_t visibleLineStart = lineStart;
        while (visible < span.end && inLineEnding(content, visible)) {
            if (content[visible] == '\n') {
                ++visibleLine;
                visibleLineStart = visible + 1;
            }
            ++visible;
        }
        if (visible > span.begin && visible < span.end) {
            match.bytes.begin = visible;
            match.line = visibleLine;
            match.column = visible - visibleLineStart + 1;
            moved = true;
        }
        matches.push_back(match);
    }
    // A moved match can now start after one that came after it.
    if (moved) {
        std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            return a.bytes.begin < b.bytes.begin;
        });
    }
    return matches;
}

SearchStatus forEachMatch(
    const IndexReader& index, const QueryNode& query, const SearchOptions& options,
    const std::function<bool(const FileMatch& file, std::string_view content)>& onFile,
    const std::atomic<bool>* cancelled) {
    // 1. Use the index to find files that might match.
    const std::vector<DocId> candidates = findCandidates(index, planCandidates(query, options));

    // 2. Check each one for real and pass on what matched.
    Evaluator evaluator(query, options);
    evaluator.useIndex(index);
    for (const DocId doc : candidates) {
        if (cancelled != nullptr && cancelled->load()) {
            return SearchStatus::Cancelled;
        }
        const std::string_view path = index.path(doc);
        bool keepGoing = true;
        index.withContent(doc, [&](std::string_view content) {
            if (!evaluator.matches(doc, path, content)) {
                return;  // the trigrams were there, but the text isn't
            }
            FileMatch file;
            file.doc = doc;
            file.path = std::string(path);
            file.matches = locate(content, evaluator.collectSpans(doc, path, content));
            keepGoing = onFile(file, content);
        });
        if (!keepGoing) {
            return SearchStatus::Truncated;
        }
    }
    return SearchStatus::Complete;
}

SearchResults search(const IndexReader& index, const QueryNode& query,
                     const SearchOptions& options) {
    SearchResults results;
    forEachMatch(index, query, options, [&](const FileMatch& file, std::string_view) {
        results.files.push_back(file);
        return true;
    });
    return results;
}

}  // namespace codesearch
