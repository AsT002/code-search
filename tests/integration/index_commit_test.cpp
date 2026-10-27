#include "codesearch/app/index_commit.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../support/temp_repo.hpp"
#include "codesearch/search/searcher.hpp"

namespace codesearch {
namespace {

using testing::TempRepo;

std::vector<std::string> indexedPaths(const MemoryIndex& index) {
    std::vector<std::string> paths;
    paths.reserve(index.docCount());
    for (DocId doc = 0; doc < index.docCount(); ++doc) {
        paths.emplace_back(index.path(doc));
    }
    return paths;
}

std::vector<std::string> matchingPaths(const MemoryIndex& index, const std::string& text,
                                       bool caseSensitive) {
    SearchOptions options;
    options.caseSensitive = caseSensitive;
    std::vector<std::string> paths;
    for (const FileMatch& file : search(index, literal(text), options).files) {
        paths.push_back(file.path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// Files `git grep` finds for the same text in the same commit.
std::vector<std::string> gitGrepPaths(TempRepo& repo, const ObjectId& commit,
                                      const std::string& text, bool caseSensitive) {
    std::vector<std::string> args = {"grep", "-l", "-z", "-F"};
    if (!caseSensitive) {
        args.emplace_back("-i");
    }
    args.insert(args.end(), {"-e", text, commit.hex()});
    // Exit code 1 means "no match", which isn't an error here.
    const GitResult result = runGit(repo.dir(), args);
    EXPECT_LE(result.exitCode, 1) << result.err;

    // Each result is "<commit>:<path>" followed by a NUL.
    std::vector<std::string> paths;
    const std::string prefix = commit.hex() + ":";
    std::size_t pos = 0;
    while (pos < result.out.size()) {
        const std::size_t end = result.out.find('\0', pos);
        const std::string item = result.out.substr(pos, end - pos);
        EXPECT_TRUE(item.starts_with(prefix)) << item;
        paths.push_back(item.substr(prefix.size()));
        pos = end + 1;
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

TEST(IndexCommit, SkipsAndCountsEachKind) {
    TempRepo repo;
    repo.write("src/a.cpp", "int a;\n");
    repo.write("src/b.cpp", "int b;\n");
    repo.write("image.png", std::string("\x89PNG\0\0", 6));
    repo.write("model.bin",
               "version https://git-lfs.github.com/spec/v1\n"
               "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n"
               "size 99999\n");
    repo.write("huge.txt", std::string((2 * 1024 * 1024) + 1, 'x'));
    repo.write("web/app.min.js", "var a=1;");
    repo.symlink("link.cpp", "src/a.cpp");
    const ObjectId first = repo.commit();
    repo.addSubmodule("third_party/lib", first);
    const ObjectId commit = repo.commit();

    const CommitIndex built = indexCommit(GitRepo(repo.dir()), commit, FileFilter());

    EXPECT_EQ(indexedPaths(built.index), (std::vector<std::string>{"src/a.cpp", "src/b.cpp"}));
    EXPECT_EQ(built.report.indexed.files, 2U);
    EXPECT_EQ(built.report.indexed.bytes, 14U);

    const std::map<SkipReason, FileCount>& skipped = built.report.skipped;
    EXPECT_EQ(skipped.size(), 6U);
    EXPECT_EQ(skipped.at(SkipReason::Binary).files, 1U);
    EXPECT_EQ(skipped.at(SkipReason::LfsPointer).files, 1U);
    EXPECT_EQ(skipped.at(SkipReason::TooLarge).files, 1U);
    EXPECT_EQ(skipped.at(SkipReason::TooLarge).bytes, (2U * 1024 * 1024) + 1);
    EXPECT_EQ(skipped.at(SkipReason::Excluded).files, 1U);
    EXPECT_EQ(skipped.at(SkipReason::Symlink).files, 1U);
    EXPECT_EQ(skipped.at(SkipReason::Submodule).files, 1U);
}

TEST(IndexCommit, IndexesFilesThatOnlyLookLikeLfsPointers) {
    const std::string version = "version https://git-lfs.github.com/spec/v1\n";
    const std::string oid =
        "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n";
    TempRepo repo;
    repo.write("cr.txt", version + "ext-0-foo sha256:00\r\n" + oid + "size 1\n");
    repo.write("twice.txt", version + version + oid + "size 1\n");
    repo.write("utf8.txt", version + "ext-0-foo a\xFF\n" + oid + "size 1\n");
    repo.write("real.bin", version + oid + "size 1\n");
    const ObjectId commit = repo.commit();

    const CommitIndex built = indexCommit(GitRepo(repo.dir()), commit, FileFilter());
    EXPECT_EQ(matchingPaths(built.index, "git-lfs.github.com", false),
              (std::vector<std::string>{"cr.txt", "twice.txt", "utf8.txt"}));
    EXPECT_EQ(built.report.skipped.at(SkipReason::LfsPointer).files, 1U);
}

TEST(IndexCommit, UsesTheCommitGivenEvenIfHeadMoves) {
    TempRepo repo;
    repo.write("a.txt", "old text\n");
    const ObjectId before = repo.commit();

    // HEAD moves after we looked it up, as if someone committed mid-build.
    repo.write("a.txt", "new text\n");
    repo.write("b.txt", "added later\n");
    repo.commit();

    const CommitIndex built = indexCommit(GitRepo(repo.dir()), before, FileFilter());
    EXPECT_EQ(indexedPaths(built.index), std::vector<std::string>{"a.txt"});
    EXPECT_EQ(matchingPaths(built.index, "old text", false), std::vector<std::string>{"a.txt"});
    EXPECT_TRUE(matchingPaths(built.index, "new text", false).empty());
}

TEST(IndexCommit, IgnoresUncommittedChanges) {
    TempRepo repo;
    repo.write("a.txt", "committed\n");
    const ObjectId commit = repo.commit();
    repo.write("a.txt", "staged\n");
    repo.writeUnstaged("b.txt", "not even staged\n");

    const GitRepo git(repo.dir());
    EXPECT_EQ(git.resolveHead(), commit);
    const CommitIndex built = indexCommit(git, commit, FileFilter());
    EXPECT_EQ(indexedPaths(built.index), std::vector<std::string>{"a.txt"});
    EXPECT_EQ(matchingPaths(built.index, "committed", false), std::vector<std::string>{"a.txt"});
    EXPECT_TRUE(matchingPaths(built.index, "staged", false).empty());
}

TEST(IndexCommit, MissingFileContentsIsAnError) {
    TempRepo repo;
    repo.write("a.txt", "a\n");
    repo.write("b.txt", "b\n");
    const ObjectId commit = repo.commit();
    repo.removeObject(repo.blobId("b.txt"));

    try {
        indexCommit(GitRepo(repo.dir()), commit, FileFilter());
        ADD_FAILURE() << "expected GitError";
    } catch (const GitError& error) {
        EXPECT_NE(std::string(error.what()).find("missing the contents of b.txt"),
                  std::string::npos)
            << error.what();
    }
}

TEST(IndexCommit, EmptyCommitGivesEmptyIndex) {
    TempRepo repo;
    const ObjectId commit = repo.commit();
    const CommitIndex built = indexCommit(GitRepo(repo.dir()), commit, FileFilter());
    EXPECT_EQ(built.index.docCount(), 0U);
    EXPECT_EQ(built.report.indexed.files, 0U);
    EXPECT_TRUE(built.report.skipped.empty());
}

TEST(IndexCommit, MatchesGitGrep) {
    TempRepo repo;
    repo.write("src/auth.go", "func Authenticate(user string) error {\n\treturn nil\n}\n");
    repo.write("src/login.go", "// calls authenticate\nfunc Login() { authenticate() }\n");
    repo.write("docs/README.md", "# Auth\r\nAUTHENTICATE users first.\r\n");
    repo.write("new\nline.txt", "authenticate in a file with a newline in its name");
    repo.write("-dash.txt", "starts with a dash; mentions auth only");
    repo.write("tab\tname.txt", "nothing to see");
    repo.write("deep/a/b/c/d.txt", "Authenticat");  // one letter short
    const ObjectId commit = repo.commit();

    const CommitIndex built = indexCommit(GitRepo(repo.dir()), commit, FileFilter());
    ASSERT_EQ(built.report.indexed.files, 7U);

    const std::vector<std::string> queries = {
        "authenticate", "Authenticate", "auth", "AUTH", "func", "nothing", "zzz", "e", "\t",
    };
    for (const std::string& query : queries) {
        for (const bool caseSensitive : {false, true}) {
            SCOPED_TRACE("query \"" + query + "\" caseSensitive=" + std::to_string(caseSensitive));
            EXPECT_EQ(matchingPaths(built.index, query, caseSensitive),
                      gitGrepPaths(repo, commit, query, caseSensitive));
        }
    }
}

}  // namespace
}  // namespace codesearch
