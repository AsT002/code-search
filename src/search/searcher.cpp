#include "codesearch/search/searcher.hpp"

#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/evaluator.hpp"

namespace codesearch {

std::vector<Match> locate(std::string_view content, std::span<const Span> spans) {
    std::vector<Match> matches;

    // Walk the content once, keeping track of which line we're on.
    std::size_t line = 1;
    std::size_t lineStart = 0;
    std::size_t scanned = 0;
    for (const Span& span : spans) {
        while (scanned < span.begin) {
            if (content[scanned] == '\n') {
                ++line;
                lineStart = scanned + 1;
            }
            ++scanned;
        }
        Match match;
        match.bytes = span;
        match.line = line;
        match.column = span.begin - lineStart + 1;
        matches.push_back(match);
    }
    return matches;
}

SearchStatus forEachMatch(
    const IndexReader& index, const QueryNode& query, const SearchOptions& options,
    const std::function<bool(const FileMatch& file, std::string_view content)>& onFile,
    const std::atomic<bool>* cancelled) {
    // 1. Use the index to find files that might match.
    const std::vector<DocId> candidates = findCandidates(index, planCandidates(query));

    // 2. Check each one for real and pass on what matched.
    const Evaluator evaluator(query, options);
    for (const DocId doc : candidates) {
        if (cancelled != nullptr && cancelled->load()) {
            return SearchStatus::Cancelled;
        }
        const std::string_view path = index.path(doc);
        bool keepGoing = true;
        index.withContent(doc, [&](std::string_view content) {
            if (!evaluator.matches(path, content)) {
                return;  // the trigrams were there, but the text isn't
            }
            FileMatch file;
            file.doc = doc;
            file.path = std::string(path);
            file.matches = locate(content, evaluator.collectSpans(path, content));
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
