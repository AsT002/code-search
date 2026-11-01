#include "codesearch/search/repo_search.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
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
    // Cutting at the line end, or a match moved past a line ending, can
    // make two spans the same, or put a longer one before a shorter one that
    // starts at the same byte. Keep them sorted, once each.
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
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
// the limit. `lineCount` is how many lines of the file have matches;
// `matches` must cover at least the first `remaining` of them. `remaining`
// is how many more results the limit allows; it goes down as results are
// found. Returns false once the limit is used up.
bool emit(const std::string& repo, const std::string& path, std::span<const Match> matches,
          std::size_t lineCount, std::string_view content, const SearchRequest& request,
          std::size_t& remaining, const std::function<void(const ResultFile& file)>& onFile) {
    if (remaining == 0) {
        return false;  // there's more than the limit allows
    }
    ResultFile file{.repo = repo, .path = path, .lines = {}};
    if (request.filesOnly || lineCount == 0) {
        --remaining;  // the file itself is the result
        onFile(file);
        return true;
    }
    const std::size_t shown = std::min(lineCount, remaining);
    file.lines = resultLines(content, matches, request.contextLines, shown);
    remaining -= shown;
    onFile(file);
    return shown == lineCount;  // false: this file had more than the limit allows
}

// The matches on the first `lines` lines that have matches, in a vector of
// just that size. (resize() would keep the memory of all of them.)
std::vector<Match> firstLines(const std::vector<Match>& matches, std::size_t lines) {
    std::size_t seen = 0;
    std::size_t previous = 0;  // lines start at 1, so 0 is never a match's line
    std::size_t end = 0;
    for (; end < matches.size(); ++end) {
        if (matches[end].line != previous) {
            if (seen == lines) {
                break;
            }
            ++seen;
            previous = matches[end].line;
        }
    }
    return {matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(end)};
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
                    return emit(target.name, match.path, match.matches, countLines(match.matches),
                                content, request, remaining, onFile);
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

// The most candidate files one task checks. Small, so the first results come
// back quickly and the work spreads evenly over the threads.
constexpr std::size_t kChunkFiles = 16;

// How many tasks may be running or finished but not yet used, per thread.
constexpr std::size_t kChunksPerThread = 4;

// About the most bytes of results the pool may hold that the caller hasn't
// taken yet, finished or still being made. Without it, a search where every
// line matches could hold the results of hundreds of files at once. It can
// go over by what each thread makes for the file it's on.
constexpr std::size_t kMaxHeldBytes = std::size_t{32} << 20U;  // 32 MiB

// Searches the repos on a thread pool. The files are checked in chunks by
// the pool's threads, while the calling thread takes the finished chunks in
// order and turns them into results. So the results come out in the same
// order as searchInTurn gives, and the limit works the same way.
//
// What's held is kept near kMaxHeldBytes in two ways:
//
// - The bytes of results are counted as they're made. Once the limit is
//   reached, no more chunks are handed out, and a task stops at the next
//   file; the rest of its chunk goes back in line, to be checked next. The
//   chunk the caller is waiting for always checks at least one file, so the
//   search keeps moving. This holds however the files change.
// - Chunks are only handed out while the results so far, plus what the
//   files already handed out will likely add at the average size so far,
//   fit. Chunks are sized the same way: 16 files normally, down to 1 when
//   every line matches. The first chunk on each thread has one file, to
//   find out. This keeps the threads from all being partway through big
//   files when the limit is reached.
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
        std::exception_ptr error;  // finding the candidates failed
    };

    // A matching file, with its result lines already worked out, so the
    // pool's threads do that work too. With a limit, only as many lines as
    // the limit allows are made.
    struct Found {
        DocId doc = 0;
        std::string path;
        std::size_t lineCount = 0;  // lines with matches, in the whole file
        // The matches on the lines in `lines`, for remaking fewer of them if
        // the limit cuts the file short. Only kept when there's a limit.
        std::vector<Match> matches;
        std::vector<ResultLine> lines;
    };

    // Up to kChunkFiles candidates of one repo, checked by one task.
    struct Chunk {
        std::size_t repo = 0;
        std::size_t begin = 0;  // a range of the repo's candidates
        std::size_t end = 0;
        std::vector<Found> found;
        std::size_t bytes = 0;  // about how much memory `found` holds
        bool cutShort = false;  // stopped early because of cancelling
        // Stopped early because too much was held; `end` is where it
        // stopped, and the rest goes up to `plannedEnd`.
        bool stoppedForMemory = false;
        std::size_t plannedEnd = 0;
        bool checkOneAnyway = false;  // the caller is waiting for this one
        std::exception_ptr error;     // what went wrong, if anything
        bool done = false;            // guarded by mutex_
    };

    void prepare();
    void fill();
    bool moreToHandOut() const { return nextRepo_ < repos_.size(); }
    std::size_t nextChunkFiles();
    void submit(std::unique_ptr<Chunk> chunk, bool next = false);
    void check(Chunk& chunk);
    bool pass(const Repo& repo, Found& found, std::size_t& remaining);
    bool cancelled() const { return request_.cancelled != nullptr && request_.cancelled->load(); }

    std::span<const SearchTarget> targets_;
    const SearchRequest& request_;
    const std::function<void(const ResultFile& file)>& onFile_;

    std::vector<Repo> repos_;
    // Chunks handed out and not taken yet, oldest first. unique_ptr, so a
    // chunk stays where it is while a task fills it.
    std::deque<std::unique_ptr<Chunk>> pending_;
    std::size_t nextRepo_ = 0;  // the first candidate not in a chunk yet
    std::size_t nextCandidate_ = 0;

    std::mutex mutex_;
    std::condition_variable chunkDone_;  // signaled each time a task ends
    // All guarded by mutex_:
    std::size_t submitted_ = 0;
    std::size_t finished_ = 0;
    std::size_t bytesSeen_ = 0;  // in all finished chunks, for sizing chunks
    std::size_t filesSeen_ = 0;
    std::size_t filesRunning_ = 0;  // in chunks handed to the pool, not done yet
    // Bytes of results made and not taken by the caller yet, counted as each
    // file's result is made.
    std::atomic<std::size_t> heldBytes_ = 0;
    std::atomic<bool> stop_ = false;  // tells running tasks to end early
};

// Finds every repo's candidates. That's fast next to checking files, so it's
// done here first.
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
        try {
            repo.evaluator->useIndex(*target.index);
            repo.candidates =
                findCandidates(*target.index, planCandidates(query, request_.options));
        } catch (const IndexFileError&) {
            // A damaged posting list. Reported when the results get to this
            // repo, as searchInTurn would, not before the repos ahead of it.
            repo.error = std::current_exception();
        }
    }
}

// How many files the next chunk gets: as many as fit in one chunk's share
// of kMaxHeldBytes, by the average result size per file so far. Until a
// chunk has finished, there's nothing to go by, so the first chunks get one
// file each (and fill() hands out only one per thread).
std::size_t ParallelSearch::nextChunkFiles() {
    const std::scoped_lock lock(mutex_);
    if (filesSeen_ == 0) {
        return 1;
    }
    const std::size_t perFile = std::max<std::size_t>(bytesSeen_ / filesSeen_, 1);
    const std::size_t share = kMaxHeldBytes / (request_.pool->size() * kChunksPerThread);
    return std::clamp<std::size_t>(share / perFile, 1, kChunkFiles);
}

// Hands out chunks until there are enough in flight, or the results they'd
// make wouldn't fit in kMaxHeldBytes. There's always at least one, so the
// search never stalls.
void ParallelSearch::fill() {
    const std::size_t window = request_.pool->size() * kChunksPerThread;
    while (moreToHandOut() && pending_.size() < window) {
        const Repo& repo = repos_[nextRepo_];
        auto chunk = std::make_unique<Chunk>();
        chunk->repo = nextRepo_;
        if (repo.error) {
            chunk->error = repo.error;  // a chunk that only reports the error
            chunk->done = true;
            pending_.push_back(std::move(chunk));
            ++nextRepo_;
            continue;
        }
        if (nextCandidate_ == repo.candidates.size()) {
            ++nextRepo_;  // nothing (more) in this repo
            nextCandidate_ = 0;
            continue;
        }
        if (!pending_.empty() && heldBytes_ >= kMaxHeldBytes) {
            return;  // wait for the caller to take some
        }
        const std::size_t files = nextChunkFiles();
        {
            const std::scoped_lock lock(mutex_);
            if (filesSeen_ == 0 && pending_.size() >= request_.pool->size()) {
                return;  // no size to go by yet; wait for the first chunk
            }
            // Also count what the files already handed out will likely add,
            // by the average so far. That keeps threads from all being
            // partway through big files when the limit is reached.
            const std::size_t perFile = filesSeen_ == 0 ? 0 : bytesSeen_ / filesSeen_;
            const std::size_t expected = heldBytes_ + (perFile * (filesRunning_ + files));
            if (!pending_.empty() && expected > kMaxHeldBytes) {
                return;
            }
        }
        chunk->begin = nextCandidate_;
        chunk->end = std::min(nextCandidate_ + files, repo.candidates.size());
        nextCandidate_ = chunk->end;
        submit(std::move(chunk));
    }
}

// Hands a chunk to the pool, last in line, or first if `next` is set.
void ParallelSearch::submit(std::unique_ptr<Chunk> chunk, bool next) {
    Chunk* task = chunk.get();
    task->plannedEnd = task->end;
    if (next) {
        pending_.push_front(std::move(chunk));
    } else {
        pending_.push_back(std::move(chunk));
    }
    const std::size_t files = task->end - task->begin;
    {
        const std::scoped_lock lock(mutex_);
        ++submitted_;
        filesRunning_ += files;
    }
    try {
        request_.pool->submit([this, task] {
            check(*task);
        });
    } catch (...) {
        // The task never got queued, so it will never finish. Don't let the
        // destructor wait for it.
        {
            const std::scoped_lock lock(mutex_);
            --submitted_;
            filesRunning_ -= files;
        }
        if (next) {
            pending_.pop_front();
        } else {
            pending_.pop_back();
        }
        throw;
    }
}

// The memory a result holds, about.
std::size_t bytesOf(const std::string& path, const std::vector<Match>& matches,
                    const std::vector<ResultLine>& lines) {
    std::size_t bytes = path.size() + (matches.size() * sizeof(Match));
    for (const ResultLine& line : lines) {
        bytes += sizeof(ResultLine) + line.text.size() + (line.matches.size() * sizeof(Span));
    }
    return bytes;
}

// Runs on a pool thread: finds which of the chunk's files match.
void ParallelSearch::check(Chunk& chunk) {
    const Repo& repo = repos_[chunk.repo];
    const IndexReader& index = *repo.target->index;
    try {
        for (std::size_t i = chunk.begin; i < chunk.end; ++i) {
            if (stop_ || cancelled()) {
                chunk.cutShort = true;
                break;
            }
            const bool mustCheck = i == chunk.begin && chunk.checkOneAnyway;
            if (!mustCheck && heldBytes_ >= kMaxHeldBytes) {
                chunk.stoppedForMemory = true;
                chunk.end = i;  // the caller hands out the rest later
                break;
            }
            const DocId doc = repo.candidates[i];
            const std::string_view path = index.path(doc);
            index.withContent(doc, [&](std::string_view content) {
                if (!repo.evaluator->matches(doc, path, content)) {
                    return;
                }
                // Made here and added once it's complete, so if something
                // throws partway (like running out of memory), the chunk
                // never holds a half-made result.
                Found found{.doc = doc, .path = std::string(path)};
                const std::vector<Match> matches =
                    locate(content, repo.evaluator->collectSpans(doc, path, content));
                found.lineCount = countLines(matches);
                if (!request_.filesOnly && found.lineCount != 0) {
                    const std::size_t kept = request_.maxResults == 0
                                                 ? found.lineCount
                                                 : std::min(found.lineCount, request_.maxResults);
                    found.lines = resultLines(content, matches, request_.contextLines, kept);
                    // Without a limit, every file fits, so they're never needed.
                    if (request_.maxResults != 0) {
                        found.matches = firstLines(matches, kept);
                    }
                }
                const std::size_t bytes = bytesOf(found.path, found.matches, found.lines);
                chunk.found.push_back(std::move(found));
                chunk.bytes += bytes;
                heldBytes_ += bytes;
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
    filesRunning_ -= chunk.plannedEnd - chunk.begin;
    bytesSeen_ += chunk.bytes;
    filesSeen_ += chunk.end - chunk.begin;
    chunkDone_.notify_all();
}

// Passes one file on, like emit() but with the lines a pool thread made.
// Those are every line the file has if it fits in what's left of the limit
// (the limit only goes down). Only a file the limit cuts short needs its
// lines made again, with fewer.
bool ParallelSearch::pass(const Repo& repo, Found& found, std::size_t& remaining) {
    const std::string& name = repo.target->name;
    const bool hasLines = !request_.filesOnly && found.lineCount != 0;
    if (hasLines && remaining != 0 && found.lineCount <= remaining) {
        remaining -= found.lineCount;
        onFile_(ResultFile{
            .repo = name,
            .path = found.path,
            .lines = std::move(found.lines),
        });
        return true;
    }
    if (!hasLines) {
        return emit(name, found.path, {}, found.lineCount, "", request_, remaining, onFile_);
    }
    bool keepGoing = false;
    try {
        repo.target->index->withContent(found.doc, [&](std::string_view content) {
            keepGoing = emit(name, found.path, found.matches, found.lineCount, content, request_,
                             remaining, onFile_);
        });
    } catch (const IndexFileError&) {
        rethrowNamed(std::current_exception(), name);
    }
    return keepGoing;
}

SearchStatus ParallelSearch::run() {
    prepare();
    std::size_t remaining =
        request_.maxResults == 0 ? std::numeric_limits<std::size_t>::max() : request_.maxResults;
    fill();
    while (!pending_.empty()) {
        // Cancelling is checked before each chunk and each file, like
        // searchInTurn checks before each candidate. Not after the last
        // one: then the search is complete.
        if (cancelled()) {
            return SearchStatus::Cancelled;
        }
        Chunk& chunk = *pending_.front();
        {
            std::unique_lock lock(mutex_);
            chunkDone_.wait(lock, [&] {
                return chunk.done;
            });
        }
        const Repo& repo = repos_[chunk.repo];
        for (Found& found : chunk.found) {
            if (cancelled()) {
                return SearchStatus::Cancelled;
            }
            if (!pass(repo, found, remaining)) {
                return SearchStatus::Truncated;
            }
        }
        // A task stops at the first damaged file, keeping the matches before
        // it. Those come first, as with searchInTurn.
        if (chunk.error) {
            rethrowNamed(chunk.error, repo.target->name);
        }
        // A chunk cancelling cut short is missing files.
        if (chunk.cutShort) {
            return SearchStatus::Cancelled;
        }
        heldBytes_ -= chunk.bytes;
        if (chunk.stoppedForMemory) {
            // The rest of the chunk comes next, before anything after it.
            auto rest = std::make_unique<Chunk>();
            rest->repo = chunk.repo;
            rest->begin = chunk.end;
            rest->end = chunk.plannedEnd;
            rest->checkOneAnyway = true;
            pending_.pop_front();
            submit(std::move(rest), true);
        } else {
            pending_.pop_front();
        }
        fill();
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
