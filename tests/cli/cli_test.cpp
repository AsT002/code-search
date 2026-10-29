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

// Runs the program with its data folder at `home`. The default is a folder
// that doesn't exist, so a test can never touch the real ~/.code-search.
ProcessResult codeSearch(const std::vector<std::string>& args,
                         const std::filesystem::path& home = "/nonexistent/code-search-home",
                         const std::vector<std::string>& extraEnv = {}) {
    std::vector<std::string> env = {"CODE_SEARCH_HOME=" + home.string()};
    env.insert(env.end(), extraEnv.begin(), extraEnv.end());
    return runProcess(CODE_SEARCH_BIN, args, env);
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
        {"search", "--path"},                            // --path without a value
        {"search", "--repo"},                            // --repo without a value
        {"search", "--path", ".", "one", "two"},         // two texts
        {"search", "--path", ".", "--", "a", "b"},       // more after "--" text
        {"search", "--path", ".", "--", "a", "--path"},  // same
        {"search", "--path", ".", "--"},                 // nothing after "--"
        {"search", "--path", ".", "--repo", "x", "a"},   // both --path and --repo
        {"search", "--index", "x", "a"},                 // gone in M4
        {"index", "--path", ".", "--out", "x"},          // gone in M4
        {"add"},
        {"add", "--name", "x"},                      // no source
        {"add", ".", "--name"},                      // --name without a value
        {"add", ".", "other"},                       // two sources
        {"add", ".", "--name", "a", "--name", "b"},  // two names
        {"add", "--bogus"},
        {"list", "x"},
        {"remove"},
        {"remove", "a", "b"},
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

// ---- Added repos ----

ProcessResult addRepo(const TempDir& home, const TempRepo& repo, const std::string& name) {
    const ProcessResult result =
        codeSearch({"add", repo.dir().string(), "--name", name}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    return result;
}

// `text` with `prefix` put before every line.
std::string prefixLines(const std::string& text, const std::string& prefix) {
    std::string result;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start) + 1;
        result += prefix + text.substr(start, end - start);
        start = end;
    }
    return result;
}

TEST(CliRepos, AddSearchListRemove) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;

    const ProcessResult added = addRepo(home, repo, "demo");
    EXPECT_TRUE(contains(added.out, "added demo")) << added.out;
    EXPECT_TRUE(contains(added.err, "indexed 3 files")) << added.err;

    const std::string path = std::filesystem::canonical(repo.dir()).string();
    ProcessResult result = codeSearch({"list"}, home.dir());
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.out.starts_with("demo  local   ")) << result.out;
    EXPECT_TRUE(result.out.ends_with("  " + path + "\n")) << result.out;

    // Searching the added repo prints what --path prints, with "demo:" in front.
    const std::vector<std::vector<std::string>> cases = {
        {"target"}, {"-l", "target"}, {"--case-sensitive", "Target"}, {"--", "->"}, {"none here"},
    };
    for (const std::vector<std::string>& args : cases) {
        const ProcessResult fromRepo = searchIn(repo, args);
        std::vector<std::string> searchArgs = {"search"};
        searchArgs.insert(searchArgs.end(), args.begin(), args.end());
        const ProcessResult fromWorkspace = codeSearch(searchArgs, home.dir());
        EXPECT_EQ(fromWorkspace.exitCode, fromRepo.exitCode) << ::testing::PrintToString(args);
        EXPECT_EQ(fromWorkspace.out, prefixLines(fromRepo.out, "demo:"))
            << ::testing::PrintToString(args);
        EXPECT_EQ(fromWorkspace.err, "");  // nothing gets indexed, so no report

        searchArgs.insert(searchArgs.begin() + 1, {"--repo", "demo"});
        EXPECT_EQ(codeSearch(searchArgs, home.dir()).out, fromWorkspace.out);
    }

    result = codeSearch({"remove", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "removed demo (" + path + " itself was not touched)\n");
    EXPECT_TRUE(std::filesystem::exists(repo.dir() / "a.txt"));

    result = codeSearch({"list"}, home.dir());
    EXPECT_EQ(result.out, "");
    EXPECT_TRUE(contains(result.err, "no repos added yet")) << result.err;
    result = codeSearch({"search", "target"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no repos added yet")) << result.err;
}

TEST(CliRepos, SearchesEveryRepoInNameOrder) {
    TempRepo first;
    first.write("x.txt", "needle\n");
    first.commit();
    TempRepo second;
    second.write("y.txt", "needle\n");
    second.commit();
    const TempDir home;
    addRepo(home, second, "zeta");
    addRepo(home, first, "alpha");

    EXPECT_EQ(codeSearch({"search", "-l", "needle"}, home.dir()).out, "alpha:x.txt\nzeta:y.txt\n");
    EXPECT_EQ(codeSearch({"search", "--repo", "zeta", "-l", "needle"}, home.dir()).out,
              "zeta:y.txt\n");
    const ProcessResult unknown = codeSearch({"search", "--repo", "nope", "needle"}, home.dir());
    EXPECT_EQ(unknown.exitCode, 2);
    EXPECT_TRUE(contains(unknown.err, "no repo named nope")) << unknown.err;
}

TEST(CliRepos, IndexKeepsItsCommit) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    addRepo(home, repo, "demo");

    repo.write("c.txt", "new target\n");
    repo.commit();
    EXPECT_EQ(codeSearch({"search", "-l", "target"}, home.dir()).out,
              "demo:a.txt\ndemo:flags.sh\n");  // until `update` arrives in M5
}

TEST(CliRepos, VerifyChecksTheIndex) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    addRepo(home, repo, "demo");

    ProcessResult result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.out, "demo: ok, 3 files")) << result.out;

    // Change one byte of a file's content. Opening still works, so search
    // can't tell, but verify can.
    const std::filesystem::directory_iterator files(home.dir() / "indexes");
    const std::filesystem::path file = files->path();
    std::string bytes = testing::readFile(file);
    bytes[bytes.find("second Target") + 2] = 'X';
    testing::writeFile(file, bytes);
    result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "index file is damaged")) << result.err;

    // A file that isn't an index at all.
    testing::writeFile(file, "hello");
    result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "not an index file")) << result.err;
    result = codeSearch({"search", "x"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "demo: not an index file")) << result.err;

    // A file that doesn't exist can't be checked at all.
    std::filesystem::remove(file);
    result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "could not open")) << result.err;
    result = codeSearch({"verify", "nope"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no repo named nope")) << result.err;
}

TEST(CliRepos, AddErrors) {
    const TempDir home;
    const TempDir notARepo;
    ProcessResult result = codeSearch({"add", notARepo.dir().string()}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "not a Git repository")) << result.err;

    result = codeSearch({"add", "https://example.com/a/b"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "only https://github.com/")) << result.err;

    result = codeSearch({"remove", "nope"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no repo named nope")) << result.err;
}

TEST(CliRepos, MissingGitIsAClearError) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    const ProcessResult result =
        codeSearch({"add", repo.dir().string()}, home.dir(), {"PATH=/nonexistent"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "Is Git installed")) << result.err;
}

TEST(CliRepos, DataGoesInHomeByDefault) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    // An empty CODE_SEARCH_HOME counts as not set.
    const ProcessResult result = codeSearch({"add", repo.dir().string(), "--name", "demo"}, "",
                                            {"HOME=" + home.dir().string()});
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(std::filesystem::exists(home.dir() / ".code-search" / "registry.json"));
}

TEST(CliErrors, Version) {
    const ProcessResult result = codeSearch({"--version"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.out.starts_with("code-search ")) << result.out;
}

}  // namespace
}  // namespace codesearch
