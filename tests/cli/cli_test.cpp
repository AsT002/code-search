// Runs the real code-search program and checks what it prints and returns.
// CODE_SEARCH_BIN is the program's full path, set by tests/CMakeLists.txt.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/temp_repo.hpp"
#include "codesearch/app/repo_manager.hpp"
#include "codesearch/index/shard_format.hpp"
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

    // In a query, '-' in front of text excludes it. Quotes make it text.
    result = searchIn(repo, {"-l", "--", "->"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "a.txt\nb.txt\n");  // the files without ">"
    result = searchIn(repo, {"-l", "\"->\""});
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
        {"search", "--path"},           // --path without a value
        {"search", "--repo"},           // --repo without a value
        {"search", "--repo", "", "a"},  // empty values would mean "all"
        {"search", "--path", "", "a"},
        {"search", "--path", ".", "one", "two"},         // two texts
        {"search", "--path", ".", "--", "a", "b"},       // more after "--" text
        {"search", "--path", ".", "--", "a", "--path"},  // same
        {"search", "--path", ".", "--"},                 // nothing after "--"
        {"search", "--path", ".", "--repo", "x", "a"},   // both --path and --repo
        {"search", "--path", ".", "-C", "x", "a"},       // -C needs a number
        {"search", "--path", ".", "-C", "-1", "a"},
        {"search", "--path", ".", "--max-results", "a"},  // no number, then no query
        {"search", "--path", ".", "--color=sometimes", "a"},
        {"search", "--index", "x", "a"},         // gone in M4
        {"index", "--path", ".", "--out", "x"},  // gone in M4
        {"add"},
        {"add", "--name", "x"},  // no source
        {"add", ".", "--name"},  // --name without a value
        {"add", ".", "--name", ""},
        {"add", ".", "other"},                       // two sources
        {"add", ".", "--name", "a", "--name", "b"},  // two names
        {"add", "--bogus"},
        {"list", "x"},
        {"update", "--rebuild"},  // no name
        {"update", "--rebuild", "--rebuild"},
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
        EXPECT_EQ(result.out, "") << ::testing::PrintToString(args);
    }
}

// With --json, bad arguments still give one whole JSON document, as every
// other error does. A "--json" that's the query, after "--", doesn't count.
TEST(Cli, BadArgumentsWithJsonGiveJson) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"search", "--json", "-C", "abc", "foo"},
             {"search", "-C", "--json", "foo"},  // not a number, so --json is --json
             {"search", "--json", "-x"},
             {"search", "a", "--json", "b"},
             // The "--" is --path's value, not the end of the options.
             {"search", "--path", "--", "--json", "-C", "abc", "foo"},
         }) {
        const ProcessResult result = codeSearch(args);
        EXPECT_EQ(result.exitCode, 2) << ::testing::PrintToString(args);
        EXPECT_EQ(result.out,
                  "{\"files\":[],\"status\":\"error\",\"error\":\"the arguments aren't valid. "
                  "See: code-search --help\"}\n")
            << ::testing::PrintToString(args);
        EXPECT_TRUE(contains(result.err, "usage:")) << result.err;
    }
    const ProcessResult result = codeSearch({"search", "--path", ".", "--", "--json", "x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_EQ(result.out, "");
}

TEST(CliErrors, NotARepo) {
    const TempRepo repo;
    std::filesystem::remove_all(repo.dir() / ".git");
    const ProcessResult result = searchIn(repo, {"x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "not a Git repository")) << result.err;
}

// A repo with nothing to search is an error (2), not "nothing found" (1).
TEST(CliErrors, EmptyRepo) {
    const TempRepo repo;
    const ProcessResult result = searchIn(repo, {"x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no commits")) << result.err;
}

// With --json, errors before the search starts still give one whole
// document, saying what's wrong.
TEST(CliErrors, EarlyErrorsAreJsonToo) {
    const TempRepo empty;
    ProcessResult result = searchIn(empty, {"--json", "x"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_EQ(
        result.out,
        "{\"files\":[],\"status\":\"error\",\"error\":\"the repository has no commits yet\"}\n");

    TempRepo repo;
    fillRepo(repo);
    result = searchIn(repo, {"--json", "(unbalanced"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(result.out.starts_with("{\"files\":[],\"status\":\"error\",\"error\":\"a ( is"))
        << result.out;

    const TempDir home;
    result = codeSearch({"search", "--json", "x"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.out, "\"error\":\"no repos added yet")) << result.out;
}

// Ctrl-C while --path is still indexing must end the JSON document, not
// leave it empty. A "git" in front of the real one makes reading files
// hang, so the Ctrl-C lands then. Like a real Ctrl-C, it goes to the whole
// process group, Git too: `set -m` gives the background job its own group,
// which `kill -INT -<pid>` signals.
TEST(CliErrors, CtrlCWhileIndexingEndsTheJson) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir bin;
    const std::string realGit = runProcess("sh", {"-c", "command -v git"}).out;
    ASSERT_FALSE(realGit.empty());
    {
        std::ofstream script(bin.dir() / "git");
        script << "#!/bin/sh\n"
                  "case \"$*\" in *cat-file*) sleep 30 ;; esac\n"
                  "exec "
               << realGit.substr(0, realGit.size() - 1) << " \"$@\"\n";
    }
    std::filesystem::permissions(bin.dir() / "git", std::filesystem::perms::owner_all);
    const std::string driver =
        "set -m\n"
        "\"$1\" search --path \"$2\" --json line 2>/dev/null &\n"
        "pid=$!\n"
        "sleep 1\n"
        "kill -INT -\"$pid\"\n"
        "wait \"$pid\"\n"
        "echo \"exit $?\" >&2\n";
    const char* path = std::getenv("PATH");  // NOLINT(concurrency-mt-unsafe)
    const ProcessResult result =
        runProcess("sh", {"-c", driver, "sh", CODE_SEARCH_BIN, repo.dir().string()},
                   {
                       "CODE_SEARCH_HOME=/nonexistent/code-search-home",
                       "PATH=" + bin.dir().string() + ":" + (path == nullptr ? "" : path),
                   });
    EXPECT_EQ(result.out, "{\"files\":[],\"status\":\"cancelled\"}\n");
    // The shell may print a job notice first.
    EXPECT_TRUE(result.err.ends_with("exit 130\n")) << result.err;
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

// Started with stdin closed, like from some schedulers and daemons. Our
// pipes to Git must not end up as fd 0.
TEST(CliErrors, WorksWithStdinClosed) {
    TempRepo repo;
    fillRepo(repo);
    // "0<&-" makes the shell close stdin before it runs the program.
    const std::vector<std::string> args = {
        "-c",
        R"(exec "$0" search --path "$1" Target 0<&-)",
        CODE_SEARCH_BIN,
        repo.dir().string(),
    };
    const ProcessResult result = runProcess("sh", args);
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "a.txt:2:8:second Target line\nflags.sh:1:15:run --flag -> target\n");
}

// ---- Query language and output ----

// A repo for the query tests: a Go file with matches 5 lines apart.
void fillQueryRepo(TempRepo& repo) {
    repo.write("a.go", "one\ntwo needle\nthree\nfour\nfive\nsix needle\nseven\n");
    repo.write("b.py", "needle in python\n");
    repo.write("c.txt", "nothing\n");
    repo.commit();
}

TEST(CliQuery, QueryLanguage) {
    TempRepo repo;
    fillQueryRepo(repo);
    EXPECT_EQ(searchIn(repo, {"-l", "needle -lang:go"}).out, "b.py\n");
    EXPECT_EQ(searchIn(repo, {"-l", "three or python"}).out, "a.go\nb.py\n");
    EXPECT_EQ(searchIn(repo, {"-l", "path:.txt or (needle path:a.)"}).out, "a.go\nc.txt\n");
    EXPECT_EQ(searchIn(repo, {"-l", "--", "-needle"}).out, "c.txt\n");
    EXPECT_EQ(searchIn(repo, {"-l", "case:yes NEEDLE"}).exitCode, 1);
    EXPECT_EQ(searchIn(repo, {"-l", "case:no NEEDLE", "--case-sensitive"}).out,
              "a.go\nb.py\n");  // case: wins over the option

    // Nothing to highlight: the file name alone.
    EXPECT_EQ(searchIn(repo, {"lang:python"}).out, "b.py\n");
    // Matches only from the branch that matched.
    EXPECT_EQ(searchIn(repo, {"(three python) or seven"}).out, "a.go:7:1:seven\n");
}

TEST(CliQuery, Regexes) {
    TempRepo repo;
    fillQueryRepo(repo);
    EXPECT_EQ(searchIn(repo, {"/^(two|six) needle$/"}).out,
              "a.go:2:1:two needle\na.go:6:1:six needle\n");
    EXPECT_EQ(searchIn(repo, {"-l", "path:/\\.py$/ /needle/"}).out, "b.py\n");
    ProcessResult result = searchIn(repo, {"--json", "/thr?e+/"});
    EXPECT_TRUE(
        contains(result.out, R"({"line":3,"text":"three","matches":[{"start":0,"end":5}]})"))
        << result.out;
    result = searchIn(repo, {"/(bad/"});
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "the regex /(bad/ isn't valid: missing )")) << result.err;
}

TEST(CliQuery, QueryErrors) {
    TempRepo repo;
    fillQueryRepo(repo);
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"(needle", "a ( is missing its )"},
        {"lang:cobol", "unknown language"},
        {"repo:x needle", "repo: only works on added repos"},
    };
    for (const auto& [query, message] : cases) {
        const ProcessResult result = searchIn(repo, {query});
        EXPECT_EQ(result.exitCode, 2) << query;
        EXPECT_TRUE(contains(result.err, message)) << result.err;
        EXPECT_EQ(result.out, "");
    }
}

TEST(CliQuery, ContextLines) {
    TempRepo repo;
    fillQueryRepo(repo);
    EXPECT_EQ(searchIn(repo, {"-C", "1", "needle path:a.go"}).out,
              "a.go-1-one\n"
              "a.go:2:5:two needle\n"
              "a.go-3-three\n"
              "--\n"
              "a.go-5-five\n"
              "a.go:6:5:six needle\n"
              "a.go-7-seven\n");
    // Groups that touch are joined, and "--" also goes between files.
    EXPECT_EQ(searchIn(repo, {"-C", "2", "needle"}).out,
              "a.go-1-one\n"
              "a.go:2:5:two needle\n"
              "a.go-3-three\n"
              "a.go-4-four\n"
              "a.go-5-five\n"
              "a.go:6:5:six needle\n"
              "a.go-7-seven\n"
              "--\n"
              "b.py:1:1:needle in python\n");
}

TEST(CliQuery, MaxResults) {
    TempRepo repo;
    fillQueryRepo(repo);
    ProcessResult result = searchIn(repo, {"--max-results", "2", "needle"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, "a.go:2:5:two needle\na.go:6:5:six needle\n");
    EXPECT_TRUE(contains(result.err, "stopped after 2 results")) << result.err;

    result = searchIn(repo, {"--max-results", "3", "needle"});
    EXPECT_EQ(result.out, "a.go:2:5:two needle\na.go:6:5:six needle\nb.py:1:1:needle in python\n");
    EXPECT_FALSE(contains(result.err, "stopped")) << result.err;

    result = searchIn(repo, {"-l", "--max-results", "1", "needle"});
    EXPECT_EQ(result.out, "a.go\n");
}

TEST(CliQuery, Json) {
    TempRepo repo;
    fillQueryRepo(repo);
    ProcessResult result = searchIn(repo, {"--json", "--max-results", "1", "needle"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out,
              "{\"files\":[\n"
              R"({"repo":"","path":"a.go","lines":[{"line":2,"text":"two needle",)"
              R"("matches":[{"start":4,"end":10}]}]})"
              "\n],\"status\":\"truncated\"}\n");

    result = searchIn(repo, {"--json", "nothing like this"});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_EQ(result.out, "{\"files\":[],\"status\":\"complete\"}\n");
}

TEST(CliQuery, Color) {
    TempRepo repo;
    fillQueryRepo(repo);
    EXPECT_EQ(searchIn(repo, {"--color=always", "three"}).out,
              "\033[35ma.go\033[0m\033[36m:\033[0m\033[32m3\033[0m\033[36m:\033[0m"
              "\033[32m1\033[0m\033[36m:\033[0m\033[1;31mthree\033[0m\n");
    // Overlapping matches: the second one's tail is colored too.
    repo.write("d.txt", "abcde\n");
    repo.commit();
    EXPECT_EQ(searchIn(repo, {"--color=always", "abc bcde"}).out,
              "\033[35md.txt\033[0m\033[36m:\033[0m\033[32m1\033[0m\033[36m:\033[0m"
              "\033[32m1\033[0m\033[36m:\033[0m\033[1;31mabc\033[0m\033[1;31mde\033[0m\n");
    // Output to a pipe isn't a terminal, so auto means no color.
    EXPECT_EQ(searchIn(repo, {"three"}).out, "a.go:3:1:three\n");
    EXPECT_EQ(searchIn(repo, {"--color=never", "three"}).out, "a.go:3:1:three\n");
}

// A file's bytes mustn't be able to control the terminal: with colors (so a
// terminal), escape sequences in the text and the path show as \xHH. Into
// a pipe without colors, the bytes stay as they are.
TEST(CliQuery, ControlBytesAreShownNotSentToTheTerminal) {
    TempRepo repo;
    repo.write("bell\x07.txt", "evil \x1b]0;title\x07 here\n");
    repo.commit();
    EXPECT_EQ(searchIn(repo, {"--color=always", "evil"}).out,
              "\033[35mbell\\x07.txt\033[0m\033[36m:\033[0m\033[32m1\033[0m\033[36m:\033[0m"
              "\033[32m1\033[0m\033[36m:\033[0m\033[1;31mevil\033[0m \\x1b]0;title\\x07 here\n");
    EXPECT_EQ(searchIn(repo, {"evil"}).out, "bell\x07.txt:1:1:evil \x1b]0;title\x07 here\n");

    // A match that ends inside a character: the whole character is
    // highlighted, and shown as it is, not escaped in halves.
    repo.write("cafe.txt", "caf\xc3\xa9 hello\n");
    repo.commit();
    EXPECT_EQ(searchIn(repo, {"--color=always", "caf\xc3"}).out,
              "\033[35mcafe.txt\033[0m\033[36m:\033[0m\033[32m1\033[0m\033[36m:\033[0m"
              "\033[32m1\033[0m\033[36m:\033[0m\033[1;31mcaf\xc3\xa9\033[0m hello\n");
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

    // repo: in the query. Each repo gets only its own part of an or.
    EXPECT_EQ(codeSearch({"search", "-l", "needle -repo:zeta"}, home.dir()).out, "alpha:x.txt\n");
    EXPECT_EQ(
        codeSearch({"search", "-l", "(repo:alpha x.txt) or (repo:zeta needle)"}, home.dir()).out,
        "zeta:y.txt\n");
    EXPECT_EQ(
        codeSearch({"search", "-l", "--repo", "alpha", "repo:zeta needle"}, home.dir()).exitCode,
        1);
    const ProcessResult unknownInQuery = codeSearch({"search", "repo:nope x"}, home.dir());
    EXPECT_EQ(unknownInQuery.exitCode, 2);
    EXPECT_TRUE(contains(unknownInQuery.err, "no repo named nope")) << unknownInQuery.err;
}

TEST(CliRepos, UpdatePicksUpNewCommits) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    addRepo(home, repo, "demo");

    repo.write("c.txt", "new target\n");
    const ObjectId commit = repo.commit();
    // The index keeps the commit it was built from until an update.
    EXPECT_EQ(codeSearch({"search", "-l", "target"}, home.dir()).out,
              "demo:a.txt\ndemo:flags.sh\n");

    ProcessResult result = codeSearch({"update", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "updated demo (new commit)\n");
    EXPECT_TRUE(contains(result.err, "commit " + commit.hex() + ": indexed 4 files")) << result.err;
    EXPECT_EQ(codeSearch({"search", "-l", "target"}, home.dir()).out,
              "demo:a.txt\ndemo:c.txt\ndemo:flags.sh\n");

    result = codeSearch({"update", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "demo is up to date (commit " + commit.hex() + ")\n");
    EXPECT_EQ(result.err, "");

    result = codeSearch({"update", "demo", "--rebuild"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "updated demo (asked to rebuild)\n");
    result = codeSearch({"update", "--rebuild", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(result.out, "updated demo (asked to rebuild)\n");

    result = codeSearch({"update", "nope"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no repo named nope")) << result.err;
    result = codeSearch({"update", "demo", "--force"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "usage:")) << result.err;
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
    EXPECT_TRUE(contains(result.err, "Rebuild it with: code-search update demo --rebuild"))
        << result.err;

    // A file that isn't an index at all.
    testing::writeFile(file, "hello");
    result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "not an index file")) << result.err;
    result = codeSearch({"search", "x"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "demo: not an index file")) << result.err;
    EXPECT_TRUE(contains(result.err, "code-search update demo --rebuild")) << result.err;

    // update notices the file doesn't open, and rebuilds it.
    result = codeSearch({"update", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.out, "updated demo (the index file can't be used")) << result.out;
    EXPECT_EQ(codeSearch({"verify", "demo"}, home.dir()).exitCode, 0);

    // A file that doesn't exist can't be checked at all. The update made a
    // new file, so look up its name again.
    std::filesystem::remove(std::filesystem::directory_iterator(home.dir() / "indexes")->path());
    result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "could not open")) << result.err;
    result = codeSearch({"verify", "nope"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "no repo named nope")) << result.err;
}

// Another repo's index copied over demo's opens fine and passes the full
// check, but it isn't demo's index. Search and verify must say so, not
// show the other repo's files as demo's.
TEST(CliRepos, AnIndexThatDoesntMatchTheRegistryIsSkipped) {
    TempRepo demo;
    fillRepo(demo);
    TempRepo other;
    other.write("other.txt", "other target\n");
    other.commit();
    const TempDir home;
    addRepo(home, demo, "demo");
    addRepo(home, other, "other");
    const Registry registry = loadRegistry(Workspace(home.dir()));
    const std::filesystem::path indexes = home.dir() / "indexes";
    std::filesystem::copy_file(indexes / registry.find("other")->index.file,
                               indexes / registry.find("demo")->index.file,
                               std::filesystem::copy_options::overwrite_existing);

    ProcessResult result = codeSearch({"verify", "demo"}, home.dir());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(contains(result.err, "the index file doesn't match the registry")) << result.err;
    // The other repo is still searched; demo is skipped with a warning.
    result = codeSearch({"search", "target"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.err,
                         "code-search: skipped demo: the index file doesn't match the registry. "
                         "Rebuild it with: code-search update demo --rebuild"))
        << result.err;
    EXPECT_EQ(result.out, "other:other.txt:1:7:other target\n");
    result = codeSearch({"search", "--json", "target"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_TRUE(contains(result.out,
                         "],\"skipped\":[{\"repo\":\"demo\",\"error\":\"demo: the "
                         "index file doesn't match the registry"))
        << result.out;
    EXPECT_TRUE(result.out.ends_with("--rebuild\"}],\"status\":\"complete\"}\n")) << result.out;
    // With only demo to search, there's nothing left: an error.
    result = codeSearch({"search", "repo:demo target"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "demo: the index file doesn't match the registry"))
        << result.err;

    result = codeSearch({"update", "demo"}, home.dir());
    EXPECT_TRUE(contains(result.out, "the index file doesn't match the registry")) << result.out;
    EXPECT_EQ(codeSearch({"verify", "demo"}, home.dir()).exitCode, 0);
}

TEST(CliRepos, DamageFoundWhileSearchingNamesTheFix) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    addRepo(home, repo, "demo");

    // Fill the posting lists with junk. The header is fine, so the file
    // opens; the damage only shows when a search reads a posting list.
    const std::filesystem::path file =
        std::filesystem::directory_iterator(home.dir() / "indexes")->path();
    std::string bytes = testing::readFile(file);
    const Section postings = decodeHeader(bytes).sections[kPostingsSection];
    bytes.replace(postings.offset, postings.length, postings.length, '\xff');
    testing::writeFile(file, bytes);

    ProcessResult result = codeSearch({"search", "target"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "demo: index file is damaged")) << result.err;
    EXPECT_TRUE(contains(result.err, "code-search update demo --rebuild")) << result.err;

    // Damage found while opening the index, before the search: JSON too.
    {
        std::string header = testing::readFile(file);
        const std::string saved = header;
        header[0] = 'X';  // the first byte of the magic number
        testing::writeFile(file, header);
        const ProcessResult opened = codeSearch({"search", "--json", "target"}, home.dir());
        EXPECT_EQ(opened.exitCode, 2);
        EXPECT_TRUE(opened.out.starts_with(
            "{\"files\":[],\"status\":\"error\",\"error\":\"demo: not an index file"))
            << opened.out;
        testing::writeFile(file, saved);
    }

    // With --json, the output is still one whole document, saying why it ended.
    result = codeSearch({"search", "--json", "target"}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(result.out.starts_with("{\"files\":[")) << result.out;
    EXPECT_TRUE(
        contains(result.out, "],\"status\":\"error\",\"error\":\"demo: index file is damaged"))
        << result.out;
    EXPECT_TRUE(result.out.ends_with("code-search update demo --rebuild\"}\n")) << result.out;

    // The file still opens, so a plain update doesn't notice. --rebuild fixes it.
    EXPECT_TRUE(contains(codeSearch({"update", "demo"}, home.dir()).out, "is up to date"));
    result = codeSearch({"update", "demo", "--rebuild"}, home.dir());
    EXPECT_EQ(result.exitCode, 0) << result.err;
    EXPECT_EQ(codeSearch({"search", "-l", "target"}, home.dir()).out,
              "demo:a.txt\ndemo:flags.sh\n");
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

// Git sets GIT_DIR and friends for hooks and aliases. They must not make
// code-search index a different repo than the one it was given.
TEST(CliRepos, GitVariablesDontChangeTheRepo) {
    TempRepo wanted;
    fillRepo(wanted);
    TempRepo other;
    other.write("other.txt", "other target\n");
    other.commit();
    const TempDir home;
    const std::string otherGit = (other.dir() / ".git").string();
    const std::vector<std::string> gitEnv = {
        "GIT_DIR=" + otherGit,
        "GIT_WORK_TREE=" + other.dir().string(),
        "GIT_OBJECT_DIRECTORY=" + otherGit + "/objects",
        "GIT_INDEX_FILE=" + otherGit + "/index",
    };
    ProcessResult result = codeSearch({"add", wanted.dir().string()}, home.dir(), gitEnv);
    ASSERT_EQ(result.exitCode, 0) << result.err;
    result = codeSearch({"search", "-l", "target"}, home.dir(), gitEnv);
    EXPECT_TRUE(contains(result.out, "a.txt")) << result.out;
    EXPECT_FALSE(contains(result.out, "other.txt")) << result.out;
    result = searchIn(wanted, {"-l", "target"});
    EXPECT_FALSE(contains(result.out, "other.txt")) << result.out;
}

// CODE_SEARCH_HOME pointing at a folder of the user's, like ~ with a tmp/
// folder in it. Nothing in it may be deleted.
TEST(CliRepos, RefusesAHomeThatHoldsOtherFiles) {
    TempRepo repo;
    fillRepo(repo);
    const TempDir home;
    std::filesystem::create_directories(home.dir() / "tmp");
    testing::writeFile(home.dir() / "tmp" / "notes.txt", "keep me");
    const ProcessResult result = codeSearch({"add", repo.dir().string()}, home.dir());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "isn't a code-search folder")) << result.err;
    EXPECT_EQ(testing::readFile(home.dir() / "tmp" / "notes.txt"), "keep me");
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

// A relative CODE_SEARCH_HOME would mean a different workspace in each folder.
TEST(CliRepos, RelativeHomeIsAnError) {
    const ProcessResult result = codeSearch({"list"}, "relative/home");
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_TRUE(contains(result.err, "CODE_SEARCH_HOME must be a full path")) << result.err;
}

TEST(CliErrors, Version) {
    const ProcessResult result = codeSearch({"--version"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.out.starts_with("code-search ")) << result.out;
}

// Runs code-search with its output on a terminal (a pty), using macOS's
// `script`. stdout and stderr both go to that terminal.
ProcessResult onATerminal(const std::vector<std::string>& args, const std::filesystem::path& home) {
    std::vector<std::string> command = {
        "-q", "/dev/null", "env", "CODE_SEARCH_HOME=" + home.string(), CODE_SEARCH_BIN,
    };
    command.insert(command.end(), args.begin(), args.end());
    return runProcess("script", command);
}

// A folder name, or a message about it, can't send the terminal escape
// sequences either: on a terminal, they show as \xHH.
TEST(CliRepos, FolderNamesAreShownNotSentToTheTerminal) {
#ifndef __APPLE__
    GTEST_SKIP() << "uses macOS's script to get a terminal";
#else
    const TempDir dir;
    const std::filesystem::path repo = dir.dir() / "ev\x1b]0;PWNED\x07il";
    std::filesystem::create_directories(repo);
    const std::string git = repo.string();
    ASSERT_EQ(runProcess("git", {"-C", git, "init", "-q"}).exitCode, 0);
    testing::writeFile(repo / "a.txt", "hello\n");
    ASSERT_EQ(runProcess("git", {"-C", git, "add", "a.txt"}).exitCode, 0);
    ASSERT_EQ(runProcess("git", {"-C", git, "-c", "user.name=t", "-c", "user.email=t@t", "commit",
                                 "-q", "-m", "x"})
                  .exitCode,
              0);
    const TempDir home;
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"add", git, "--name", "evil"},
             {"list"},
             {"add", git, "--name", "again"},  // the "already added" error, on stderr
             {"remove", "evil"},
         }) {
        SCOPED_TRACE(::testing::PrintToString(args));
        const ProcessResult result = onATerminal(args, home.dir());
        EXPECT_FALSE(contains(result.out, "\x1b]0;")) << result.out;
        EXPECT_TRUE(contains(result.out, "ev\\x1b]0;PWNED\\x07il")) << result.out;
    }
    // Into a pipe, the bytes stay as they are.
    EXPECT_TRUE(contains(codeSearch({"add", git, "--name", "evil"}, home.dir()).out, "ev\x1b]0;"));
#endif
}

// verify's messages name the index file, inside the data folder, which can
// be named anything too.
TEST(CliRepos, VerifyMessagesAreShownNotSentToTheTerminal) {
#ifndef __APPLE__
    GTEST_SKIP() << "uses macOS's script to get a terminal";
#else
    TempRepo repo;
    fillRepo(repo);
    const TempDir dir;
    const std::filesystem::path home = dir.dir() / "ho\x1b]0;PWNED\x07me";
    ASSERT_EQ(codeSearch({"add", repo.dir().string(), "--name", "demo"}, home).exitCode, 0);
    testing::writeFile(std::filesystem::directory_iterator(home / "indexes")->path(), "hello");
    for (const std::string& name : {std::string("demo"), std::string("no\x1b]0;PWNED\x07pe")}) {
        const ProcessResult result = onATerminal({"verify", name}, home);
        EXPECT_FALSE(contains(result.out, "\x1b]0;")) << result.out;
        EXPECT_TRUE(contains(result.out, "\\x1b]0;PWNED\\x07")) << result.out;
    }
#endif
}

}  // namespace
}  // namespace codesearch
