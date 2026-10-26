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

SearchResults search(const IndexReader& index, const QueryNode& query,
                     const SearchOptions& options) {
    // 1. Use the index to find files that might match.
    const TrigramQuery plan = planCandidates(query);
    const std::vector<DocId> candidates = findCandidates(index, plan);

    // 2. Check each one for real and record what matched.
    const Evaluator evaluator(query, options);
    SearchResults results;
    for (const DocId doc : candidates) {
        index.withContent(doc, [&](std::string_view content) {
            if (!evaluator.matches(content)) {
                return;  // the trigrams were there, but the text isn't
            }
            FileMatch file;
            file.doc = doc;
            file.path = std::string(index.path(doc));
            file.matches = locate(content, evaluator.collectSpans(content));
            results.files.push_back(file);
        });
    }
    return results;
}

}  // namespace codesearch
