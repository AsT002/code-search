#include "codesearch/search/repo_search.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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
    mutable int reads_ = 0;  // mutable: withContent is const, but counting changes this
};

MemoryIndex manyFiles(int count) {
    IndexBuilder builder;
    for (int i = 0; i < count; ++i) {
        builder.add("f" + std::to_string(1000 + i) + ".go", "needle\nneedle again\n");
    }
    return builder.build();
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

}  // namespace
}  // namespace codesearch
