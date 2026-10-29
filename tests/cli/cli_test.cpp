// Runs the real code-search program and checks what it prints and returns.
// CODE_SEARCH_BIN is the program's full path, set by tests/CMakeLists.txt.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/temp_repo.hpp"
#include "codesearch/util/process.hpp"

namespace codesearch {
namespace {

using testing::TempDir;
using testing::TempRepo;

ProcessResult codeSearch(const std::vector<std::string>& args) {
    return runProcess(CODE_SEARCH_BIN, args);
}

ProcessResult searchIn(const TempRepo& repo, std::vector<std::string> args) {
    args.insert(args.begin(), {"search", "--path", repo.dir().string()});
    return codeSearch(args);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The repo most CLI tests search.
void fillRepo(TempRepo& repo) {
    repo.write("a.txt", "first line\nsecond Target line\n");
    repo.write("b.txt", "nothing here\r\n");
    repo.write("flags.sh", "run --flag -> target\n");
    repo.commit();
}

TEST(Cli, PrintsMatchingLines) {
    TempRepo repo;
    fillRepo(repo);
    const ProcessResult result = searchIn(repo, {"target"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "a.txt:2:8:second Target line\nflags.sh:1:15:run --flag -> target\n");
    EXPECT_TRUE(contains(result.err, "indexed 3 files")) << result.err;
}

TEST(Cli, ListsFilesOnly) {
    TempRepo repo;
    fillRepo(repo);
    const ProcessResult result = searchIn(repo, {"-l", "target"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "a.txt\nflags.sh\n");
}

TEST(Cli, CaseSensitive) {
    TempRepo repo;
    fillRepo(repo);
    const ProcessResult result = searchIn(repo, {"--case-sensitive", "-l", "Target"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "a.txt\n");
}

TEST(Cli, NothingFoundExitsWithOne) {
    TempRepo repo;
    fillRepo(repo);
    const ProcessResult result = searchIn(repo, {"no such text"});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_EQ(result.out, "");
}

TEST(Cli, DoubleDashAllowsTextStartingWithADash) {
    TempRepo repo;
    fillRepo(repo);
    ProcessResult result = searchIn(repo, {"-l", "--", "--flag"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "flags.sh\n");

    result = searchIn(repo, {"-l", "--", "->"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "flags.sh\n");

    // Without "--" it looks like an unknown option.
    result = searchIn(repo, {"--flag"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "usage:"));
}

TEST(Cli, BadArgumentsPrintUsage) {
    const std::vector<std::vector<std::string>> cases = {
        {},
        {"search"},
        {"search", "text"},                              // no --path
        {"search", "--path"},                            // --path without a value
        {"search", "--path", ".", "one", "two"},         // two texts
        {"search", "--path", ".", "--", "a", "b"},       // more after "--" text
        {"search", "--path", ".", "--", "a", "--path"},  // same
        {"search", "--path", ".", "--"},                 // nothing after "--"
        {"search", "--path", ".", "--index", "x", "a"},  // both --path and --index
        {"search", "--index", "x"},                      // no text
        {"index", "--path", "."},                        // no --out
        {"index", "--out", "x"},                         // no --path
        {"index", "--path", ".", "--out"},               // --out without a value
        {"index", "--path", ".", "--out", "x", "--z", "y"},
        {"verify"},
        {"verify", "a", "b"},
        {"unknown"},
    };
    for (const std::vector<std::string>& args : cases) {
        const ProcessResult result = codeSearch(args);
        EXPECT_EQ(result.exitCode, 2) << ::testing::PrintToString(args);
        EXPECT_TRUE(contains(result.err, "usage:")) << ::testing::PrintToString(args);
    }
}

TEST(CliErrors, NotARepo) {
    const TempRepo repo;
    std::filesystem::remove_all(repo.dir() / ".git");
    const ProcessResult result = searchIn(repo, {"x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "not a Git repository")) << result.err;
}

TEST(CliErrors, EmptyRepo) {
    const TempRepo repo;
    const ProcessResult result = searchIn(repo, {"x"});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "no commits")) << result.err;
}

TEST(CliErrors, UnreadableHeadCommitIsAnError) {
    TempRepo repo;
    repo.write("a.txt", "a\n");
    repo.removeObject(repo.commit());

    const ProcessResult result = searchIn(repo, {"a"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "can't be read as a commit")) << result.err;
}

TEST(CliErrors, MissingFileContentsIsAnError) {
    TempRepo repo;
    repo.write("a.txt", "needle\n");
    repo.write("b.txt", "other\n");
    repo.commit();
    repo.removeObject(repo.blobId("b.txt"));

    const ProcessResult result = searchIn(repo, {"needle"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_EQ(result.out, "");
    EXPECT_TRUE(contains(result.err, "missing the contents of b.txt")) << result.err;
}

TEST(CliErrors, ClosedOutputIsAnError) {
    // Far more output than a pipe holds, so the program can't finish writing
    // before we close the pipe.
    TempRepo repo;
    std::string content;
    for (int i = 0; i < 20000; ++i) {
        content += "match\n";
    }
    repo.write("many.txt", content);
    repo.commit();

    Process search(CODE_SEARCH_BIN, {"search", "--path", repo.dir().string(), "match"});
    const int exitCode = search.wait();  // closes our end of its output first
    EXPECT_EQ(exitCode, 2);
    EXPECT_TRUE(contains(search.errorOutput(), "could not write the output"))
        << search.errorOutput();
}

// ---- Saved index files ----

// Saves the repo's index to `file`.
void indexTo(const TempRepo& repo, const std::string& file) {
    const ProcessResult result =
        codeSearch({"index", "--path", repo.dir().string(), "--out", file});
    ASSERT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.err, "indexed 3 files")) << result.err;
}

ProcessResult searchFile(const std::string& file, std::vector<std::string> args) {
    args.insert(args.begin(), {"search", "--index", file});
    return codeSearch(args);
}

TEST(CliIndex, SavedIndexGivesTheSameOutput) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir dir;
    const std::string file = dir.file("repo.idx").string();
    indexTo(repo, file);

    const std::vector<std::vector<std::string>> cases = {
        {"target"}, {"-l", "target"}, {"--case-sensitive", "Target"}, {"--", "->"}, {"none here"},
    };
    for (const std::vector<std::string>& args : cases) {
        const ProcessResult fromRepo = searchIn(repo, args);
        const ProcessResult fromFile = searchFile(file, args);
        EXPECT_EQ(fromFile.exitCode, fromRepo.exitCode) << ::testing::PrintToString(args);
        EXPECT_EQ(fromFile.out, fromRepo.out) << ::testing::PrintToString(args);
        EXPECT_EQ(fromFile.err, "");  // nothing gets indexed, so no report
    }
}

TEST(CliIndex, SavedIndexKeepsItsCommit) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir dir;
    const std::string file = dir.file("repo.idx").string();
    indexTo(repo, file);

    repo.write("c.txt", "new target\n");
    repo.commit();
    EXPECT_EQ(searchFile(file, {"-l", "target"}).out, "a.txt\nflags.sh\n");
    EXPECT_EQ(searchIn(repo, {"-l", "target"}).out, "a.txt\nc.txt\nflags.sh\n");
}

TEST(CliIndex, VerifyChecksTheFile) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir dir;
    const std::filesystem::path file = dir.file("repo.idx");
    indexTo(repo, file.string());

    ProcessResult result = codeSearch({"verify", file.string()});
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.out, "ok, 3 files")) << result.out;

    // Change one byte of a file's content. Opening still works, so search
    // can't tell, but verify can.
    std::string bytes = testing::readFile(file);
    bytes[bytes.find("second Target") + 2] = 'X';
    testing::writeFile(file, bytes);
    result = codeSearch({"verify", file.string()});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "index file is damaged")) << result.err;

    // A file that isn't an index at all.
    testing::writeFile(file, "hello");
    result = codeSearch({"verify", file.string()});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "not an index file")) << result.err;
    result = codeSearch({"search", "--index", file.string(), "x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "not an index file")) << result.err;

    // A file that doesn't exist can't be checked at all.
    result = codeSearch({"verify", dir.file("missing.idx").string()});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "could not open")) << result.err;
}

TEST(CliIndex, EmptyRepoWritesNothing) {
    const TempRepo repo;
    const TempDir dir;
    const ProcessResult result =
        codeSearch({"index", "--path", repo.dir().string(), "--out", dir.file("x.idx").string()});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "no commits")) << result.err;
    EXPECT_FALSE(std::filesystem::exists(dir.file("x.idx")));
}

TEST(CliErrors, Version) {
    const ProcessResult result = codeSearch({"--version"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.out.starts_with("code-search ")) << result.out;
}

}  // namespace
}  // namespace codesearch
