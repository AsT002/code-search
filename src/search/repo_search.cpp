#include "codesearch/search/repo_search.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "codesearch/query/planner.hpp"

namespace codesearch {

namespace {

// Where each line starts: line n (counting from 1) starts at starts[n - 1].
// A '\n' at the very end doesn't start another line.
std::vector<std::size_t> lineStarts(std::string_view content) {
    std::vector<std::size_t> starts = {0};
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n' && i + 1 < content.size()) {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

// The text of line `number`, without its '\n' and a CR before it.
std::string_view lineText(std::string_view content, const std::vector<std::size_t>& starts,
                          std::size_t number) {
    const std::size_t start = starts[number - 1];
    std::size_t end = number < starts.size() ? starts[number] : content.size();
    // Only a CR right before the '\n' is part of the line ending. A CR at
    // the very end of the file, with no '\n' after it, is text.
    if (end > start && content[end - 1] == '\n') {
        --end;
        if (end > start && content[end - 1] == '\r') {
            --end;
        }
    }
    return content.substr(start, end - start);
}

// A range of lines to show, both ends included.
struct LineRange {
    std::size_t first = 0;
    std::size_t last = 0;
};

// How many different lines the matches are on.
std::size_t countLines(std::span<const Match> matches) {
    std::size_t count = 0;
    std::size_t previous = 0;  // lines start at 1, so 0 is never a match's line
    for (const Match& match : matches) {
        if (match.line != previous) {
            ++count;
            previous = match.line;
        }
    }
    return count;
}

// The numbers of the lines with matches, in order, at most `max` of them.
std::vector<std::size_t> matchingLines(std::span<const Match> matches, std::size_t max) {
    std::vector<std::size_t> lines;
    for (const Match& match : matches) {
        if (!lines.empty() && lines.back() == match.line) {
            continue;
        }
        if (lines.size() == max) {
            break;
        }
        lines.push_back(match.line);
    }
    return lines;
}

// The lines to show: each of `lines` with `context` lines either side,
// joined where they touch or overlap.
std::vector<LineRange> rangesAround(const std::vector<std::size_t>& lines, std::size_t context,
                                    std::size_t lastLine) {
    context = std::min(context, lastLine);  // so a huge `context` can't overflow
    std::vector<LineRange> ranges;
    for (const std::size_t line : lines) {
        const std::size_t first = line > context ? line - context : 1;
        const std::size_t last = std::min(lastLine, line + context);
        if (!ranges.empty() && first <= ranges.back().last + 1) {
            ranges.back().last = last;
        } else {
            ranges.push_back({.first = first, .last = last});
        }
    }
    return ranges;
}

// The matches on line `number`, as byte ranges inside its text, cut at the
// end of the text. `next` is the first match not looked at yet; this moves
// it past the line's matches.
std::vector<Span> matchesOnLine(std::span<const Match> matches, std::size_t& next,
                                std::size_t number, std::size_t lineStart, std::size_t textSize) {
    while (next < matches.size() && matches[next].line < number) {
        ++next;
    }
    std::vector<Span> spans;
    const std::size_t textEnd = lineStart + textSize;
    for (; next < matches.size() && matches[next].line == number; ++next) {
        const Span bytes = matches[next].bytes;
        const std::size_t begin = std::min(bytes.begin, textEnd) - lineStart;
        const std::size_t end = std::min(bytes.end, textEnd) - lineStart;
        spans.push_back(Span{begin, end});
    }
    return spans;
}

}  // namespace

std::vector<ResultLine> resultLines(std::string_view content, std::span<const Match> matches,
                                    std::size_t context, std::size_t maxMatchLines) {
    const std::vector<std::size_t> starts = lineStarts(content);
    const std::vector<std::size_t> kept = matchingLines(matches, maxMatchLines);

    std::vector<ResultLine> out;
    std::size_t next = 0;  // the first match not looked at yet
    for (const LineRange& range : rangesAround(kept, context, starts.size())) {
        for (std::size_t number = range.first; number <= range.last; ++number) {
            const std::string_view text = lineText(content, starts, number);
            std::vector<Span> spans =
                matchesOnLine(matches, next, number, starts[number - 1], text.size());
            // A line past the limit can show up as context; its matches aren't kept.
            if (!std::binary_search(kept.begin(), kept.end(), number)) {
                spans.clear();
            }
            out.push_back({
                .number = number,
                .text = std::string(text),
                .matches = spans,
            });
        }
    }
    return out;
}

namespace {

// Searches one repo. `remaining` is how many more results the limit allows;
// it goes down as results are found.
SearchStatus searchOne(const SearchTarget& target, const QueryNode& query,
                       const SearchRequest& request, std::size_t& remaining,
                       const std::function<void(const ResultFile& file)>& onFile) {
    return forEachMatch(
        *target.index, query, request.options,
        [&](const FileMatch& match, std::string_view content) {
            if (remaining == 0) {
                return false;  // there's more than the limit allows
            }
            ResultFile file{.repo = target.name, .path = match.path, .lines = {}};
            if (request.filesOnly || match.matches.empty()) {
                --remaining;  // the file itself is the result
                onFile(file);
                return true;
            }
            const std::size_t found = countLines(match.matches);
            const std::size_t shown = std::min(found, remaining);
            file.lines = resultLines(content, match.matches, request.contextLines, shown);
            remaining -= shown;
            onFile(file);
            return shown == found;  // false: this file had more than the limit allows
        },
        request.cancelled);
}

}  // namespace

SearchStatus searchRepos(std::span<const SearchTarget> targets, const SearchRequest& request,
                         const std::function<void(const ResultFile& file)>& onFile) {
    std::size_t remaining =
        request.maxResults == 0 ? std::numeric_limits<std::size_t>::max() : request.maxResults;

    for (const SearchTarget& target : targets) {
        const QueryNode query = resolveRepo(request.query, target.name);
        if (query == constant(false)) {
            continue;  // nothing in this repo can match
        }
        SearchStatus status = SearchStatus::Complete;
        try {
            status = searchOne(target, query, request, remaining, onFile);
        } catch (const IndexFileError& error) {
            if (target.name.empty()) {
                throw;
            }
            throw RepoIndexError(target.name, error.what());
        }
        if (status != SearchStatus::Complete) {
            return status;
        }
    }
    return SearchStatus::Complete;
}

}  // namespace codesearch
