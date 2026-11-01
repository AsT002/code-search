#include "codesearch/search/repo_search.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/evaluator.hpp"

namespace codesearch {

namespace {

// The text of the line starting at `start`, without its '\n' and a CR
// before it. Sets `next` to where the line after it starts, or npos if this
// is the last line.
std::string_view lineAt(std::string_view content, std::size_t start, std::size_t& next) {
    const std::size_t newline = content.find('\n', start);
    std::size_t end = newline == std::string_view::npos ? content.size() : newline;
    // A '\n' at the very end doesn't start another line.
    next = newline == std::string_view::npos || newline + 1 == content.size()
               ? std::string_view::npos
               : newline + 1;
    // Only a CR right before the '\n' is part of the line ending. A CR at
    // the very end of the file, with no '\n' after it, is text.
    if (newline != std::string_view::npos && end > start && content[end - 1] == '\r') {
        --end;
    }
    return content.substr(start, end - start);
}

// Where the line `back` lines before the one starting at `start` starts.
// There must be at least that many lines before it.
std::size_t linesBack(std::string_view content, std::size_t start, std::size_t back) {
    for (; back > 0; --back) {
        // start - 1 is the '\n' ending the line before. Its start is just
        // after the '\n' before that one, or 0.
        const std::size_t newline =
            start >= 2 ? content.rfind('\n', start - 2) : std::string_view::npos;
        start = newline == std::string_view::npos ? 0 : newline + 1;
    }
    return start;
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
// joined where they touch or overlap. A range can run past the last line;
// resultLines stops there.
std::vector<LineRange> rangesAround(const std::vector<std::size_t>& lines, std::size_t context) {
    std::vector<LineRange> ranges;
    for (const std::size_t line : lines) {
        const std::size_t first = line > context ? line - context : 1;
        // Written this way so a huge `context` can't overflow.
        const std::size_t last = context > std::numeric_limits<std::size_t>::max() - line
                                     ? std::numeric_limits<std::size_t>::max()
                                     : line + context;
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
    const std::vector<std::size_t> kept = matchingLines(matches, maxMatchLines);

    std::vector<ResultLine> out;
    std::size_t next = 0;  // the first match not looked at yet
    for (const LineRange& range : rangesAround(kept, context)) {
        // Every range is built around a matching line: the first one in it
        // is the first kept line at or after range.first. Its start is known
        // from a match on it, since a match's column counts from there.
        const std::size_t firstKept = *std::lower_bound(kept.begin(), kept.end(), range.first);
        while (matches[next].line < firstKept) {
            ++next;
        }
        const Match& anchor = matches[next];
        std::size_t start =
            linesBack(content, anchor.bytes.begin - (anchor.column - 1), firstKept - range.first);

        for (std::size_t number = range.first; number <= range.last; ++number) {
            std::size_t after = 0;
            const std::string_view text = lineAt(content, start, after);
            std::vector<Span> spans = matchesOnLine(matches, next, number, start, text.size());
            // A line past the limit can show up as context; its matches aren't kept.
            if (!std::binary_search(kept.begin(), kept.end(), number)) {
                spans.clear();
            }
            out.push_back({
                .number = number,
                .text = std::string(text),
                .matches = std::move(spans),
            });
            if (after == std::string_view::npos) {
                return out;  // that was the last line
            }
            start = after;
        }
    }
    return out;
}

namespace {

// Turns one matching file into a result and passes it to onFile, keeping to
// the limit. `remaining` is how many more results the limit allows; it goes
// down as results are found. Returns false once the limit is used up.
bool emit(const std::string& repo, const FileMatch& match, std::string_view content,
          const SearchRequest& request, std::size_t& remaining,
          const std::function<void(const ResultFile& file)>& onFile) {
    if (remaining == 0) {
        return false;  // there's more than the limit allows
    }
    ResultFile file{.repo = repo, .path = match.path, .lines = {}};
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
}

// An IndexFileError from searching `repo`, with the repo's name in front.
[[noreturn]] void rethrowNamed(const std::exception_ptr& error, const std::string& repo) {
    try {
        std::rethrow_exception(error);
    } catch (const IndexFileError& damaged) {
        if (repo.empty()) {
            throw;
        }
        throw RepoIndexError(repo, damaged.what());
    }
}

// Searches the repos one after another, on this thread.
SearchStatus searchInTurn(std::span<const SearchTarget> targets, const SearchRequest& request,
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
            status = forEachMatch(
                *target.index, query, request.options,
                [&](const FileMatch& match, std::string_view content) {
                    return emit(target.name, match, content, request, remaining, onFile);
                },
                request.cancelled);
        } catch (const IndexFileError&) {
            rethrowNamed(std::current_exception(), target.name);
        }
        if (status != SearchStatus::Complete) {
            return status;
        }
    }
    return SearchStatus::Complete;
}

// How many candidate files one task checks. Small, so the first results come
// back quickly and the work spreads evenly over the threads.
constexpr std::size_t kChunkFiles = 16;

// How many tasks may be running or finished but not yet used, per thread.
// This keeps memory small when the caller reads the results slowly.
constexpr std::size_t kChunksPerThread = 4;

// Searches the repos on a thread pool. The files are checked in chunks by
// the pool's threads, while the calling thread takes the finished chunks in
// order and turns them into results. So the results come out in the same
// order as searchInTurn gives, and the limit works the same way.
class ParallelSearch {
public:
    ParallelSearch(std::span<const SearchTarget> targets, const SearchRequest& request,
                   const std::function<void(const ResultFile& file)>& onFile)
        : targets_(targets), request_(request), onFile_(onFile) {}

    // Waits for the tasks still running, since they use this object.
    ~ParallelSearch() {
        stop_ = true;
        std::unique_lock lock(mutex_);
        chunkDone_.wait(lock, [this] {
            return finished_ == submitted_;
        });
    }

    ParallelSearch(const ParallelSearch&) = delete;
    ParallelSearch& operator=(const ParallelSearch&) = delete;
    ParallelSearch(ParallelSearch&&) = delete;
    ParallelSearch& operator=(ParallelSearch&&) = delete;

    SearchStatus run();

private:
    // One repo, set up to be checked by many threads at once.
    struct Repo {
        const SearchTarget* target = nullptr;
        // optional, because an Evaluator can only be made from a query.
        std::optional<Evaluator> evaluator;
        std::vector<DocId> candidates;
    };

    // A matching file, with its result lines for every match already
    // worked out, so the pool's threads do that work too.
    struct Found {
        FileMatch match;
        std::vector<ResultLine> lines;
    };

    // Up to kChunkFiles candidates of one repo, checked by one task.
    struct Chunk {
        std::size_t repo = 0;
        std::size_t begin = 0;  // a range of the repo's candidates
        std::size_t end = 0;
        std::vector<Found> found;
        std::exception_ptr error;  // what went wrong, if anything
        bool done = false;         // guarded by mutex_
    };

    void prepare();
    void submit(std::size_t chunk);
    void check(Chunk& chunk);
    bool pass(const Repo& repo, Found& found, std::size_t& remaining);
    bool cancelled() const { return request_.cancelled != nullptr && request_.cancelled->load(); }

    std::span<const SearchTarget> targets_;
    const SearchRequest& request_;
    const std::function<void(const ResultFile& file)>& onFile_;

    std::vector<Repo> repos_;
    std::vector<Chunk> chunks_;

    std::mutex mutex_;
    std::condition_variable chunkDone_;  // signaled each time a task ends
    std::size_t submitted_ = 0;          // guarded by mutex_
    std::size_t finished_ = 0;           // guarded by mutex_
    std::atomic<bool> stop_ = false;     // tells running tasks to end early
};

// Finds every repo's candidates, and splits them into chunks in result order.
// Finding candidates is fast next to checking files, so it's done here first.
void ParallelSearch::prepare() {
    repos_.reserve(targets_.size());
    for (const SearchTarget& target : targets_) {
        const QueryNode query = resolveRepo(request_.query, target.name);
        if (query == constant(false)) {
            continue;  // nothing in this repo can match
        }
        Repo& repo = repos_.emplace_back();
        repo.target = &target;
        repo.evaluator.emplace(query, request_.options);
        const std::size_t index = repos_.size() - 1;
        try {
            repo.evaluator->useIndex(*target.index);
            repo.candidates = findCandidates(*target.index, planCandidates(query));
        } catch (const IndexFileError&) {
            // A damaged posting list. Report it when the results get to this
            // repo, as searchInTurn would, not before the repos ahead of it.
            Chunk& chunk = chunks_.emplace_back();
            chunk.repo = index;
            chunk.error = std::current_exception();
            chunk.done = true;
            continue;
        }
        for (std::size_t begin = 0; begin < repo.candidates.size(); begin += kChunkFiles) {
            Chunk& chunk = chunks_.emplace_back();
            chunk.repo = index;
            chunk.begin = begin;
            chunk.end = std::min(begin + kChunkFiles, repo.candidates.size());
        }
    }
}

void ParallelSearch::submit(std::size_t chunk) {
    if (chunks_[chunk].done) {
        return;  // a chunk that only reports an error
    }
    {
        const std::scoped_lock lock(mutex_);
        ++submitted_;
    }
    request_.pool->submit([this, chunk] {
        check(chunks_[chunk]);
    });
}

// Runs on a pool thread: finds which of the chunk's files match.
void ParallelSearch::check(Chunk& chunk) {
    const Repo& repo = repos_[chunk.repo];
    const IndexReader& index = *repo.target->index;
    try {
        for (std::size_t i = chunk.begin; i < chunk.end && !stop_ && !cancelled(); ++i) {
            const DocId doc = repo.candidates[i];
            const std::string_view path = index.path(doc);
            index.withContent(doc, [&](std::string_view content) {
                if (!repo.evaluator->matches(doc, path, content)) {
                    return;
                }
                Found& found = chunk.found.emplace_back();
                found.match = FileMatch{
                    .doc = doc,
                    .path = std::string(path),
                    .matches = locate(content, repo.evaluator->collectSpans(doc, path, content)),
                };
                if (!request_.filesOnly && !found.match.matches.empty()) {
                    const std::size_t all = std::numeric_limits<std::size_t>::max();
                    found.lines =
                        resultLines(content, found.match.matches, request_.contextLines, all);
                }
            });
        }
    } catch (...) {  // a task must not throw; run() passes it on
        chunk.error = std::current_exception();
    }
    // Signal while still holding the lock. Once it's let go, the search may
    // see that every task is done and be destroyed, condition variable and
    // all, so this task must not touch it after that.
    const std::scoped_lock lock(mutex_);
    chunk.done = true;
    ++finished_;
    chunkDone_.notify_all();
}

// Passes one file on, like emit() but with the lines a pool thread made.
// Only a file the limit cuts short needs its lines made again, with fewer.
bool ParallelSearch::pass(const Repo& repo, Found& found, std::size_t& remaining) {
    const FileMatch& match = found.match;
    const bool hasLines = !request_.filesOnly && !match.matches.empty();
    if (hasLines && remaining != 0 && countLines(match.matches) <= remaining) {
        remaining -= countLines(match.matches);
        onFile_(ResultFile{
            .repo = repo.target->name,
            .path = match.path,
            .lines = std::move(found.lines),
        });
        return true;
    }
    if (!hasLines) {
        return emit(repo.target->name, match, "", request_, remaining, onFile_);
    }
    bool keepGoing = false;
    try {
        repo.target->index->withContent(match.doc, [&](std::string_view content) {
            keepGoing = emit(repo.target->name, match, content, request_, remaining, onFile_);
        });
    } catch (const IndexFileError&) {
        rethrowNamed(std::current_exception(), repo.target->name);
    }
    return keepGoing;
}

SearchStatus ParallelSearch::run() {
    prepare();
    std::size_t remaining =
        request_.maxResults == 0 ? std::numeric_limits<std::size_t>::max() : request_.maxResults;
    const std::size_t window = request_.pool->size() * kChunksPerThread;
    for (std::size_t i = 0; i < std::min(window, chunks_.size()); ++i) {
        submit(i);
    }

    for (std::size_t next = 0; next < chunks_.size(); ++next) {
        Chunk& chunk = chunks_[next];
        {
            std::unique_lock lock(mutex_);
            chunkDone_.wait(lock, [&] {
                return chunk.done;
            });
        }
        // A chunk cut short by cancelling isn't complete, so check first.
        if (cancelled()) {
            return SearchStatus::Cancelled;
        }
        const Repo& repo = repos_[chunk.repo];
        if (chunk.error) {
            rethrowNamed(chunk.error, repo.target->name);
        }
        for (Found& found : chunk.found) {
            if (!pass(repo, found, remaining)) {
                return SearchStatus::Truncated;
            }
        }
        chunk.found = {};  // let go of the memory
        if (next + window < chunks_.size()) {
            submit(next + window);
        }
    }
    return SearchStatus::Complete;
}

}  // namespace

SearchStatus searchRepos(std::span<const SearchTarget> targets, const SearchRequest& request,
                         const std::function<void(const ResultFile& file)>& onFile) {
    if (request.pool == nullptr) {
        return searchInTurn(targets, request, onFile);
    }
    ParallelSearch search(targets, request, onFile);
    return search.run();
}

}  // namespace codesearch
