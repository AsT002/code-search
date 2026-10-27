// Runs the real code-search program and checks what it prints and returns.
// CODE_SEARCH_BIN is the program's full path, set by tests/CMakeLists.txt.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../support/temp_repo.hpp"
#include "codesearch/util/process.hpp"

namespace codesearch {
namespace {

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

TEST(CliErrors, Version) {
    const ProcessResult result = codeSearch({"--version"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.out.starts_with("code-search ")) << result.out;
}

}  // namespace
}  // namespace codesearch
