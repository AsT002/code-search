#include "codesearch/search/repo_search.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../support/allocations.hpp"
#include "../support/failing_pool.hpp"
#include "codesearch/index/memory_index.hpp"
#include "codesearch/search/evaluator.hpp"

namespace codesearch {
namespace {

// The lines resultLines gives for every match of `text` in `content`.
std::vector<ResultLine> linesFor(std::string_view content, const std::string& text,
                                 std::size_t context, std::size_t maxMatchLines = 100) {
    const Evaluator evaluator(literal(text), {});
    const std::vector<Match> matches = locate(content, evaluator.collectSpans("", content));
    return resultLines(content, matches, context, maxMatchLines);
}

ResultLine hit(std::size_t number, std::string text, std::vector<Span> matches) {
    return {.number = number, .text = std::move(text), .matches = std::move(matches)};
}

ResultLine around(std::size_t number, std::string text) {
    return {.number = number, .text = std::move(text), .matches = {}};
}

constexpr std::string_view kSix = "one\ntwo xx\nthree\nfour\nfive xx xx\r\nsix\n";

TEST(ResultLines, MatchingLinesOnly) {
    EXPECT_EQ(linesFor(kSix, "xx", 0), (std::vector<ResultLine>{
                                           hit(2, "two xx", {{4, 6}}),
                                           hit(5, "five xx xx", {{5, 7}, {8, 10}}),
                                       }));
}

TEST(ResultLines, ContextJoinsGroupsThatTouch) {
    EXPECT_EQ(linesFor(kSix, "xx", 1), (std::vector<ResultLine>{
                                           around(1, "one"),
                                           hit(2, "two xx", {{4, 6}}),
                                           around(3, "three"),
                                           around(4, "four"),
                                           hit(5, "five xx xx", {{5, 7}, {8, 10}}),
                                           around(6, "six"),
                                       }));
    // Context never goes past the first or last line, even if it's huge.
    EXPECT_EQ(linesFor(kSix, "xx", static_cast<std::size_t>(-1)).size(), 6U);
}

TEST(ResultLines, LimitCountsMatchingLines) {
    EXPECT_EQ(linesFor(kSix, "xx", 0, 1), (std::vector<ResultLine>{hit(2, "two xx", {{4, 6}})}));
    // A matching line past the limit can show up as context, without its matches.
    const std::vector<ResultLine> lines = linesFor(kSix, "xx", 3, 1);
    ASSERT_EQ(lines.size(), 5U);
    EXPECT_EQ(lines[4], around(5, "five xx xx"));
}

TEST(ResultLines, OnlyACrBeforeANewlineIsHidden) {
    EXPECT_EQ(linesFor("xyz\r", "xyz", 0), (std::vector<ResultLine>{hit(1, "xyz\r", {{0, 3}})}));
    EXPECT_EQ(linesFor("a\rb\r\n", "b", 0), (std::vector<ResultLine>{hit(1, "a\rb", {{2, 3}})}));
}

// A match that starts in a line ending is shown where its text starts, on
// the next line, not as an empty highlight at the end of the line before.
TEST(ResultLines, AMatchStartingInALineEndingIsShownOnTheNextLine) {
    EXPECT_EQ(linesFor("foo\r\nbar", "\nba", 0),
              (std::vector<ResultLine>{hit(2, "bar", {{0, 2}})}));
    EXPECT_EQ(linesFor("foo\r\nbar", "\r\nb", 0),
              (std::vector<ResultLine>{hit(2, "bar", {{0, 1}})}));
    EXPECT_EQ(linesFor("x\n\nyz", "\n\ny", 0), (std::vector<ResultLine>{hit(3, "yz", {{0, 1}})}));
    // Only line endings: nothing to show, so it stays at the end of its line.
    EXPECT_EQ(linesFor("ab\ncd", "\n", 0), (std::vector<ResultLine>{hit(1, "ab", {{2, 2}})}));
    // A lone CR is text, so a match starting there stays on its line.
    EXPECT_EQ(linesFor("a\rb", "\rb", 0), (std::vector<ResultLine>{hit(1, "a\rb", {{1, 3}})}));
}

// Spans that become the same after a match is moved or cut show once, in
// order of start, then end.
TEST(ResultLines, SpansAreSortedAndUniqueAfterMovingAndCutting) {
    const auto lines = [](std::string_view content, const QueryNode& query) {
        const Evaluator evaluator(query, {});
        const std::vector<Match> matches = locate(content, evaluator.collectSpans("", content));
        return resultLines(content, matches, 0, 100);
    };
    EXPECT_EQ(lines("x\nfoo\n", anyOf({literal("\nfoo"), literal("foo")})),
              (std::vector<ResultLine>{hit(2, "foo", {{0, 3}})}));
    EXPECT_EQ(lines("x\nfoo\n", anyOf({literal("\nfoo"), literal("fo")})),
              (std::vector<ResultLine>{hit(2, "foo", {{0, 2}, {0, 3}})}));
    EXPECT_EQ(lines("x\nfoo\n", anyOf({literal("x\n"), literal("x")})),
              (std::vector<ResultLine>{hit(1, "x", {{0, 1}})}));
}

TEST(ResultLines, MatchesAreCutAtTheEndOfTheLine) {
    // The match runs into the next line and over the CR, which isn't shown.
    EXPECT_EQ(linesFor("ab\r\ncd", "b\r\nc", 0), (std::vector<ResultLine>{hit(1, "ab", {{1, 2}})}));
    EXPECT_EQ(linesFor("no newline at the end", "end", 0),
              (std::vector<ResultLine>{hit(1, "no newline at the end", {{18, 21}})}));
}

// An index that counts how many files were read, to check that searches
// with a limit stop early.
class CountingIndex : public IndexReader {
public:
    explicit CountingIndex(const IndexReader& inner) : inner_(&inner) {}

    std::uint32_t docCount() const override { return inner_->docCount(); }
    std::optional<PostingCursor> postings(Trigram trigram) const override {
        return inner_->postings(trigram);
    }
    std::string_view path(DocId doc) const override { return inner_->path(doc); }
    void withContent(DocId doc, const std::function<void(std::string_view)>& fn) const override {
        ++reads_;
        inner_->withContent(doc, fn);
    }

    int reads() const { return reads_; }

private:
    const IndexReader* inner_;
    // mutable: withContent is const, but counting changes this. Atomic,
    // since threads of a pool count at the same time.
    mutable std::atomic<int> reads_ = 0;
};

MemoryIndex manyFiles(int count) {
    IndexBuilder builder;
    for (int i = 0; i < count; ++i) {
        builder.add("f" + std::to_string(1000 + i) + ".go", "needle\nneedle again\n");
    }
    return builder.build();
}

// Plays a slow reader of results: waits until the pool has made all it's
// allowed to make, that is until the memory in use stops changing. That
// doesn't depend on how busy the machine is, like a fixed pause would.
void waitForThePoolToSettle() {
    std::size_t last = gLive;
    int unchanged = 0;
    for (int i = 0; i < 1000 && unchanged < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const std::size_t now = gLive;
        unchanged = now == last ? unchanged + 1 : 0;
        last = now;
    }
}

// "repo:path" for each file found, in order.
std::vector<std::string> found(std::span<const SearchTarget> targets, const SearchRequest& request,
                               SearchStatus* status = nullptr) {
    std::vector<std::string> out;
    const SearchStatus result = searchRepos(targets, request, [&](const ResultFile& file) {
        out.push_back(file.repo + ":" + file.path + ":" + std::to_string(file.lines.size()));
    });
    if (status != nullptr) {
        *status = result;
    }
    return out;
}

TEST(SearchRepos, EachRepoGetsItsPartOfTheQuery) {
    IndexBuilder a;
    a.add("x.txt", "foo bar");
    const MemoryIndex indexA = a.build();
    IndexBuilder b;
    b.add("y.txt", "foo bar");
    const MemoryIndex indexB = b.build();
    const std::vector<SearchTarget> targets = {
        {.name = "a", .index = &indexA},
        {.name = "b", .index = &indexB},
    };

    SearchRequest request;
    request.query =
        anyOf({allOf({repoFilter("a"), literal("foo")}), allOf({repoFilter("b"), literal("bar")})});
    std::vector<ResultFile> files;
    EXPECT_EQ(searchRepos(targets, request,
                          [&](const ResultFile& file) {
                              files.push_back(file);
                          }),
              SearchStatus::Complete);
    ASSERT_EQ(files.size(), 2U);
    // In a, only foo is highlighted; in b, only bar.
    EXPECT_EQ(files[0].repo, "a");
    EXPECT_EQ(files[0].lines, (std::vector<ResultLine>{hit(1, "foo bar", {{0, 3}})}));
    EXPECT_EQ(files[1].repo, "b");
    EXPECT_EQ(files[1].lines, (std::vector<ResultLine>{hit(1, "foo bar", {{4, 7}})}));
}

TEST(SearchRepos, SkipsReposThatCantMatch) {
    const MemoryIndex index = manyFiles(3);
    CountingIndex counted(index);
    const std::vector<SearchTarget> targets = {{.name = "a", .index = &counted}};
    SearchRequest request;
    request.query = allOf({repoFilter("b"), literal("needle")});
    EXPECT_TRUE(found(targets, request).empty());
    EXPECT_EQ(counted.reads(), 0);
}

TEST(SearchRepos, LimitStopsReadingEarly) {
    const MemoryIndex index = manyFiles(200);
    CountingIndex counted(index);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &counted}};
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 3;  // each file has 2 matching lines

    SearchStatus status = SearchStatus::Complete;
    EXPECT_EQ(found(targets, request, &status),
              (std::vector<std::string>{"r:f1000.go:2", "r:f1001.go:1"}));
    EXPECT_EQ(status, SearchStatus::Truncated);
    EXPECT_EQ(counted.reads(), 2);  // not 200
}

TEST(SearchRepos, ExactlyAtTheLimitIsComplete) {
    const MemoryIndex index = manyFiles(2);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 4;
    SearchStatus status = SearchStatus::Truncated;
    EXPECT_EQ(found(targets, request, &status).size(), 2U);
    EXPECT_EQ(status, SearchStatus::Complete);

    request.maxResults = 3;
    EXPECT_EQ(found(targets, request, &status).size(), 2U);
    EXPECT_EQ(status, SearchStatus::Truncated);
}

TEST(SearchRepos, FilesCountAsOneWithoutHighlights) {
    const MemoryIndex index = manyFiles(5);
    CountingIndex counted(index);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &counted}};
    SearchStatus status = SearchStatus::Complete;

    SearchRequest request;
    request.query = langFilter("go");  // nothing to highlight
    request.maxResults = 2;
    EXPECT_EQ(found(targets, request, &status),
              (std::vector<std::string>{"r:f1000.go:0", "r:f1001.go:0"}));
    EXPECT_EQ(status, SearchStatus::Truncated);

    request.query = literal("needle");
    request.filesOnly = true;
    request.maxResults = 3;
    EXPECT_EQ(found(targets, request, &status).size(), 3U);
    EXPECT_EQ(status, SearchStatus::Truncated);
}

TEST(SearchRepos, LimitCarriesAcrossRepos) {
    const MemoryIndex index = manyFiles(1);
    const std::vector<SearchTarget> targets = {
        {.name = "a", .index = &index},
        {.name = "b", .index = &index},
    };
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 3;
    SearchStatus status = SearchStatus::Complete;
    EXPECT_EQ(found(targets, request, &status),
              (std::vector<std::string>{"a:f1000.go:2", "b:f1000.go:1"}));
    EXPECT_EQ(status, SearchStatus::Truncated);
}

TEST(SearchRepos, CancelStopsBeforeTheNextFile) {
    const MemoryIndex index = manyFiles(5);
    CountingIndex counted(index);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &counted}};
    const std::atomic<bool> cancelled = true;
    SearchRequest request;
    request.query = literal("needle");
    request.cancelled = &cancelled;
    SearchStatus status = SearchStatus::Complete;
    EXPECT_TRUE(found(targets, request, &status).empty());
    EXPECT_EQ(status, SearchStatus::Cancelled);
    EXPECT_EQ(counted.reads(), 0);
}

// The pool gives the same results in the same order, across many chunks
// and several repos, with and without a limit.
TEST(SearchReposOnAPool, SameResultsInTheSameOrder) {
    const MemoryIndex big = manyFiles(300);
    const MemoryIndex small = manyFiles(5);
    const std::vector<SearchTarget> targets = {
        {.name = "a", .index = &small},
        {.name = "b", .index = &big},
        {.name = "c", .index = &small},
    };
    ThreadPool pool(4);
    for (const std::size_t limit : {0U, 1U, 7U, 100U, 610U, 1000U}) {
        SCOPED_TRACE(limit);
        SearchRequest request;
        request.query = literal("needle");
        request.maxResults = limit;
        SearchStatus inTurn = SearchStatus::Cancelled;
        const std::vector<std::string> expected = found(targets, request, &inTurn);
        request.pool = &pool;
        SearchStatus onPool = SearchStatus::Cancelled;
        EXPECT_EQ(found(targets, request, &onPool), expected);
        EXPECT_EQ(onPool, inTurn);
    }
}

// With a limit, the pool only reads a few chunks past the last result.
TEST(SearchReposOnAPool, LimitStopsReadingEarly) {
    const MemoryIndex index = manyFiles(1000);
    CountingIndex counted(index);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &counted}};
    ThreadPool pool(1);
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 3;
    request.pool = &pool;
    SearchStatus status = SearchStatus::Complete;
    EXPECT_EQ(found(targets, request, &status),
              (std::vector<std::string>{"r:f1000.go:2", "r:f1001.go:1"}));
    EXPECT_EQ(status, SearchStatus::Truncated);
    // 4 chunks of 16 files in flight with one thread, and the results need
    // only the first 2 files read again.
    EXPECT_LE(counted.reads(), (4 * 16) + 2);
}

// With a limit, the pool makes only as many result lines per file as the
// limit allows. Before, it made every line of every file it read ahead,
// over 2 MiB here for one result.
TEST(SearchReposOnAPool, LimitBoundsWhatIsPrepared) {
    IndexBuilder builder;
    const std::string line = std::string(2000, 'x') + " needle\n";
    for (int i = 0; i < 32; ++i) {
        std::string content;
        for (int n = 0; n < 32; ++n) {
            content += line;
        }
        builder.add("f" + std::to_string(i), content);
    }
    const MemoryIndex index = builder.build();
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 1;
    request.pool = &pool;

    std::size_t lines = 0;
    gAllocated = 0;
    gCounting = true;
    const SearchStatus status = searchRepos(targets, request, [&](const ResultFile& file) {
        lines += file.lines.size();
    });
    gCounting = false;
    EXPECT_EQ(status, SearchStatus::Truncated);
    EXPECT_EQ(lines, 1U);
    EXPECT_LT(gAllocated.load(), std::size_t{512} << 10U) << "bytes allocated";
}

// The matches kept for a limited search must not keep the memory of all
// of them: here every file has 4000 matches and only 1 is shown. Keeping
// them all for the 64 files read ahead took about 16 MiB.
TEST(SearchReposOnAPool, LimitBoundsWhatIsKept) {
    IndexBuilder builder;
    std::string content;
    for (int n = 0; n < 4000; ++n) {
        content += "needle\n";
    }
    for (int i = 0; i < 64; ++i) {
        builder.add("f" + std::to_string(i), content);
    }
    const MemoryIndex index = builder.build();
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("needle");
    request.maxResults = 1;
    request.pool = &pool;

    const std::size_t before = gLive;
    gPeak = before;
    gCounting = true;
    const SearchStatus status = searchRepos(targets, request, [](const ResultFile&) {});
    gCounting = false;
    EXPECT_EQ(status, SearchStatus::Truncated);
    EXPECT_LT(gPeak - before, std::size_t{4} << 20U) << "most bytes live at once";
}

// Without a limit, results waiting for the caller are kept to about
// 32 MiB. Before, the pool took on 256 files at once and held all their
// results: here about 200 MiB, while one thread needs a few. The caller
// is slow to take the first result, like a terminal printing a lot, so the
// pool gets ahead as far as it's allowed to.
TEST(SearchReposOnAPool, NoLimitStillBoundsWhatIsHeld) {
    IndexBuilder builder;
    std::string content;
    for (int n = 0; n < 1024; ++n) {
        content += std::string(31, 'e') + "\n";
    }
    for (int i = 0; i < 128; ++i) {
        builder.add("f" + std::to_string(i), content);
    }
    const MemoryIndex index = builder.build();
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("e");
    request.pool = &pool;

    std::size_t files = 0;
    const std::size_t before = gLive;
    gPeak = before;
    gCounting = true;
    const SearchStatus status = searchRepos(targets, request, [&](const ResultFile& /*file*/) {
        if (++files == 1) {
            waitForThePoolToSettle();
        }
    });
    gCounting = false;
    EXPECT_EQ(status, SearchStatus::Complete);
    EXPECT_EQ(files, 128U);
    EXPECT_LT(gPeak - before, std::size_t{64} << 20U) << "most bytes live at once";
}

// Results sizes measured so far can mislead: here the first 64 files have
// no matches, so the files after them looked cheap, and the pool took on
// over a hundred files where every line matches (about 65 MiB). What's held
// must be limited by the bytes actually made, not by a guess.
TEST(SearchReposOnAPool, HeldResultsStayBoundedWhenFilesChange) {
    IndexBuilder builder;
    const std::string none(std::size_t{32} << 10U, 'x');
    std::string dense;
    for (int n = 0; n < 1024; ++n) {
        dense += std::string(31, 'e') + "\n";
    }
    for (int i = 0; i < 64; ++i) {
        builder.add("a" + std::to_string(i), none);
    }
    // 128 files of about 0.6 MB of results each: far more than the limit,
    // and few enough to stay quick under ThreadSanitizer.
    for (int i = 0; i < 128; ++i) {
        builder.add("b" + std::to_string(i), dense);
    }
    const MemoryIndex index = builder.build();
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("e");
    request.pool = &pool;

    std::vector<std::string> paths;
    SearchStatus status = SearchStatus::Cancelled;
    const std::size_t before = gLive;
    gPeak = before;
    gCounting = true;
    // Tasks stop partway when too much is held, and the rest of their files
    // are checked later. If that ever stopped moving, this would hang.
    testing::failIfItHangs([&] {
        status = searchRepos(targets, request, [&](const ResultFile& file) {
            paths.push_back(file.path);
            if (paths.size() == 1) {
                waitForThePoolToSettle();
            }
        });
    });
    gCounting = false;
    EXPECT_EQ(status, SearchStatus::Complete);
    std::vector<std::string> expected;
    expected.reserve(128);
    for (int i = 0; i < 128; ++i) {
        expected.push_back("b" + std::to_string(i));
    }
    EXPECT_EQ(paths, expected);  // all of them, in order
    // 32 MiB of results waiting, plus what 4 threads hold for the file each
    // is working on.
    EXPECT_LT(gPeak - before, std::size_t{56} << 20U) << "most bytes live at once";
}

// The chunk the caller waits for must check a file even when chunks after
// it already hold the most allowed: those can't be taken before it, so
// otherwise nothing would ever move again. Here a slow file holds up an
// early chunk while the later chunks fill up the limit.
TEST(SearchReposOnAPool, KeepsMovingWhenLaterChunksHoldTheLimit) {
    std::string dense;
    for (int n = 0; n < 2048; ++n) {
        dense += std::string(31, 'e') + "\n";
    }
    IndexBuilder builder;
    for (int i = 0; i < 5; ++i) {
        builder.add("a" + std::to_string(i), "xxx");  // no match
    }
    for (int i = 0; i < 80; ++i) {
        builder.add("b" + std::to_string(i), dense);
    }
    const MemoryIndex inner = builder.build();
    class SlowFile : public CountingIndex {
    public:
        using CountingIndex::CountingIndex;
        void withContent(DocId doc,
                         const std::function<void(std::string_view)>& fn) const override {
            if (doc == 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            CountingIndex::withContent(doc, fn);
        }
    };
    const SlowFile index(inner);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("e");
    request.pool = &pool;
    std::size_t files = 0;
    SearchStatus status = SearchStatus::Cancelled;
    testing::failIfItHangs([&] {
        status = searchRepos(targets, request, [&](const ResultFile& /*file*/) {
            ++files;
        });
    });
    EXPECT_EQ(status, SearchStatus::Complete);
    EXPECT_EQ(files, 80U);
}

// Cancelled partway through the last chunk, before any file in it matched:
// files were left unchecked, so the search is cancelled, not complete.
TEST(SearchReposOnAPool, CancelPartwayThroughTheLastChunk) {
    // Each file has every trigram of "needle", but only the last two have
    // the text. The pool's first chunks get one file each (one per thread),
    // so the third file starts a bigger chunk: the last one.
    IndexBuilder builder;
    builder.add("a", "needlx eedle");
    builder.add("b", "needlx eedle");
    builder.add("c", "needlx eedle");
    builder.add("d", "needle");
    builder.add("e", "needle");
    const MemoryIndex inner = builder.build();
    std::atomic<bool> cancelled = false;
    // Reading the third file cancels the search, as Ctrl-C at that moment
    // would. It waits a moment first, so the caller is already waiting for
    // that chunk.
    class CancelOnFirstRead : public CountingIndex {
    public:
        CancelOnFirstRead(const IndexReader& index, std::atomic<bool>& cancel)
            : CountingIndex(index), cancel_(&cancel) {}
        void withContent(DocId doc,
                         const std::function<void(std::string_view)>& fn) const override {
            if (doc == 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *cancel_ = true;
            }
            CountingIndex::withContent(doc, fn);
        }

    private:
        std::atomic<bool>* cancel_;
    };
    const CancelOnFirstRead index(inner, cancelled);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(2);
    for (ThreadPool* threads : {static_cast<ThreadPool*>(nullptr), &pool}) {
        SCOPED_TRACE(threads == nullptr ? "one thread" : "pool");
        cancelled = false;
        SearchRequest request;
        request.query = literal("needle");
        request.cancelled = &cancelled;
        request.pool = threads;
        SearchStatus status = SearchStatus::Complete;
        EXPECT_TRUE(found(targets, request, &status).empty());
        EXPECT_EQ(status, SearchStatus::Cancelled);
    }
}

// Cancelling while the last result is handled leaves nothing undone, so the
// search is complete, the same as on one thread.
TEST(SearchReposOnAPool, CancelDuringTheLastResultIsComplete) {
    const MemoryIndex index = manyFiles(2);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(2);
    for (ThreadPool* threads : {static_cast<ThreadPool*>(nullptr), &pool}) {
        SCOPED_TRACE(threads == nullptr ? "one thread" : "pool");
        std::atomic<bool> cancelled = false;
        SearchRequest request;
        request.query = literal("needle");
        request.cancelled = &cancelled;
        request.pool = threads;
        std::size_t files = 0;
        const SearchStatus status = searchRepos(targets, request, [&](const ResultFile&) {
            if (++files == 2) {
                cancelled = true;
            }
        });
        EXPECT_EQ(files, 2U);
        EXPECT_EQ(status, SearchStatus::Complete);
    }
}

// If a pool thread runs out of memory partway through a file, the search
// must throw std::bad_alloc or give correct results, never a half-made
// result. With one pool thread, the Nth allocation on it is the same each
// run, so failing each one in turn tries every point.
TEST(SearchReposOnAPool, OutOfMemoryOnAThreadNeverGivesAHalfResult) {
    const MemoryIndex index = manyFiles(20);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(1);
    gMainThread = std::this_thread::get_id();
    for (const std::size_t limit : {0U, 1U, 3U}) {
        SearchRequest request;
        request.query = literal("needle");
        request.maxResults = limit;
        SearchStatus expectedStatus = SearchStatus::Complete;
        const std::vector<std::string> expected = found(targets, request, &expectedStatus);
        request.pool = &pool;
        for (long failAt = 1; failAt <= 400; ++failAt) {
            SCOPED_TRACE("limit " + std::to_string(limit) + ", allocation " +
                         std::to_string(failAt));
            std::vector<std::string> got;
            gFailIn = failAt;
            try {
                const SearchStatus status =
                    searchRepos(targets, request, [&](const ResultFile& file) {
                        got.push_back(file.repo + ":" + file.path + ":" +
                                      std::to_string(file.lines.size()));
                    });
                gFailIn = 0;
                EXPECT_EQ(status, expectedStatus);
                EXPECT_EQ(got, expected);
            } catch (const std::bad_alloc&) {
                gFailIn = 0;
                // What came out before the error must be the start of the results.
                ASSERT_LE(got.size(), expected.size());
                EXPECT_TRUE(std::equal(got.begin(), got.end(), expected.begin()));
            }
        }
    }
}

// Damage past the limit is never reached one file at a time, so the pool
// mustn't report it either. Without a limit, the files before it come first.
TEST(SearchReposOnAPool, DamageAfterTheLimitIsntReported) {
    class BrokenAtTwo : public CountingIndex {
    public:
        using CountingIndex::CountingIndex;
        void withContent(DocId doc,
                         const std::function<void(std::string_view)>& fn) const override {
            if (doc == 2) {
                throw IndexFileError("index file is damaged: test");
            }
            CountingIndex::withContent(doc, fn);
        }
    };
    const MemoryIndex index = manyFiles(5);
    const BrokenAtTwo broken(index);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &broken}};
    ThreadPool pool(2);
    for (ThreadPool* threads : {static_cast<ThreadPool*>(nullptr), &pool}) {
        SCOPED_TRACE(threads == nullptr ? "one thread" : "pool");
        SearchRequest request;
        request.query = literal("needle");
        request.pool = threads;
        request.maxResults = 1;
        SearchStatus status = SearchStatus::Complete;
        EXPECT_EQ(found(targets, request, &status), (std::vector<std::string>{"r:f1000.go:1"}));
        EXPECT_EQ(status, SearchStatus::Truncated);

        request.maxResults = 0;
        std::vector<std::string> before;
        EXPECT_THROW(searchRepos(targets, request,
                                 [&](const ResultFile& file) {
                                     before.push_back(file.path);
                                 }),
                     RepoIndexError);
        EXPECT_EQ(before, (std::vector<std::string>{"f1000.go", "f1001.go"}));
    }
}

// Cancelling while the pool has files waiting stops before the next one.
TEST(SearchReposOnAPool, CancelBetweenFilesOfAChunk) {
    const MemoryIndex index = manyFiles(3);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(2);
    std::atomic<bool> cancelled = false;
    SearchRequest request;
    request.query = literal("needle");
    request.cancelled = &cancelled;
    request.pool = &pool;
    std::size_t files = 0;
    const SearchStatus status = searchRepos(targets, request, [&](const ResultFile& /*file*/) {
        ++files;
        cancelled = true;
    });
    EXPECT_EQ(status, SearchStatus::Cancelled);
    EXPECT_EQ(files, 1U);
}

// A task that can't be queued must not leave the search waiting for it.
TEST(SearchReposOnAPool, FailedSubmitIsAnErrorNotAHang) {
    const MemoryIndex index = manyFiles(200);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    testing::FailingPool pool(2, 1);
    SearchRequest request;
    request.query = literal("needle");
    request.pool = &pool;
    testing::failIfItHangs([&] {
        EXPECT_THROW(searchRepos(targets, request, [](const ResultFile&) {}), std::bad_alloc);
    });
}

TEST(SearchReposOnAPool, Cancel) {
    const MemoryIndex index = manyFiles(100);
    const std::vector<SearchTarget> targets = {{.name = "r", .index = &index}};
    ThreadPool pool(2);
    const std::atomic<bool> cancelled = true;
    SearchRequest request;
    request.query = literal("needle");
    request.cancelled = &cancelled;
    request.pool = &pool;
    SearchStatus status = SearchStatus::Complete;
    EXPECT_TRUE(found(targets, request, &status).empty());
    EXPECT_EQ(status, SearchStatus::Cancelled);
}

// When the caller can't take results (its output closed, say), it throws,
// and the search stops right away instead of reading every file.
TEST(SearchRepos, AnErrorFromTheCallerStopsTheSearch) {
    const MemoryIndex index = manyFiles(1000);
    ThreadPool pool(2);
    for (ThreadPool* threads : {static_cast<ThreadPool*>(nullptr), &pool}) {
        SCOPED_TRACE(threads == nullptr ? "one thread" : "pool");
        CountingIndex counted(index);
        const std::vector<SearchTarget> targets = {{.name = "r", .index = &counted}};
        SearchRequest request;
        request.query = literal("needle");
        request.pool = threads;
        EXPECT_THROW(searchRepos(targets, request,
                                 [](const ResultFile&) {
                                     throw std::runtime_error("could not write the output");
                                 }),
                     std::runtime_error);
        // One thread reads the first file; the pool, a few chunks ahead.
        EXPECT_LE(counted.reads(), threads == nullptr ? 1 : 200);
    }
}

TEST(SearchRepos, DamageNamesTheRepo) {
    class BrokenIndex : public CountingIndex {
    public:
        using CountingIndex::CountingIndex;
        void withContent(DocId /*doc*/,
                         const std::function<void(std::string_view)>& /*fn*/) const override {
            throw IndexFileError("index file is damaged: test");
        }
    };
    const MemoryIndex index = manyFiles(1);
    const BrokenIndex broken(index);
    std::vector<SearchTarget> targets = {{.name = "r", .index = &broken}};
    SearchRequest request;
    request.query = literal("needle");
    try {
        found(targets, request);
        ADD_FAILURE() << "expected an error";
    } catch (const RepoIndexError& error) {
        EXPECT_EQ(error.repo(), "r");
        EXPECT_STREQ(error.what(), "r: index file is damaged: test");
    }
    targets[0].name = "";  // a folder that isn't added: no name to add
    EXPECT_THROW(found(targets, request), IndexFileError);
}

// On the pool, damage is reported when the results reach the damaged repo,
// after the results of the repos before it.
TEST(SearchReposOnAPool, DamageComesInOrder) {
    class BrokenIndex : public CountingIndex {
    public:
        using CountingIndex::CountingIndex;
        void withContent(DocId /*doc*/,
                         const std::function<void(std::string_view)>& /*fn*/) const override {
            throw IndexFileError("index file is damaged: test");
        }
    };
    const MemoryIndex index = manyFiles(40);
    const BrokenIndex broken(index);
    const std::vector<SearchTarget> targets = {
        {.name = "good", .index = &index},
        {.name = "bad", .index = &broken},
    };
    ThreadPool pool(4);
    SearchRequest request;
    request.query = literal("needle");
    request.pool = &pool;
    std::size_t before = 0;
    try {
        searchRepos(targets, request, [&](const ResultFile& file) {
            EXPECT_EQ(file.repo, "good");
            ++before;
        });
        ADD_FAILURE() << "expected an error";
    } catch (const RepoIndexError& error) {
        EXPECT_EQ(error.repo(), "bad");
    }
    EXPECT_EQ(before, 40U);
}

}  // namespace
}  // namespace codesearch
