#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/temp_repo.hpp"
#include "codesearch/git/git_repo.hpp"
#include "codesearch/git/git_runner.hpp"
#include "codesearch/util/object_id.hpp"

namespace codesearch {
namespace {

using testing::TempDir;
using testing::TempRepo;

constexpr std::string_view kSampleHex = "0123456789abcdef0123456789abcdef01234567";

TEST(RunGit, ReturnsOutputAndExitCode) {
    const TempRepo repo;
    const GitResult ok = runGit(repo.dir(), {"rev-parse", "--is-inside-work-tree"});
    EXPECT_EQ(ok.exitCode, 0);
    EXPECT_EQ(ok.out, "true\n");

    const GitResult bad = runGit(repo.dir(), {"rev-parse", "--verify", "no-such-branch"});
    EXPECT_NE(bad.exitCode, 0);
    EXPECT_FALSE(bad.err.empty());
}

TEST(RunGit, PassesArgumentsWithoutAShell) {
    const TempRepo repo;
    // A shell would run the command after ";". Git must get it as one argument.
    const std::string weird = "x; touch pwned";
    const GitResult result = runGit(repo.dir(), {"check-ref-format", "--normalize", weird});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(std::filesystem::exists(repo.dir() / "pwned"));
}

TEST(GitCatFile, TalksBothWays) {
    TempRepo repo;
    repo.write("a.txt", "hello\n");
    const ObjectId commit = repo.commit();

    Process git("git", gitArgs(repo.dir(), {"cat-file", "--batch-check"}));
    git.write(commit.hex() + "\n");
    std::string line;
    ASSERT_TRUE(git.readLine(line));
    EXPECT_TRUE(line.starts_with(commit.hex() + " commit "));
    git.closeInput();
    EXPECT_FALSE(git.readLine(line));
    EXPECT_EQ(git.wait(), 0);
}

TEST(GitRepo, RejectsFolderOutsideGit) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    // The temp folder itself is not a repo (unless someone made it one).
    if (runGit(dir, {"rev-parse"}).exitCode == 0) {
        GTEST_SKIP() << "temp folder is inside a Git repo";
    }
    EXPECT_THROW(GitRepo{dir}, GitError);
}

TEST(GitRepo, RejectsSha256) {
    const TempRepo repo("sha256");
    EXPECT_THROW(GitRepo{repo.dir()}, GitError);
}

TEST(GitRepo, EmptyRepoHasNoHead) {
    const TempRepo repo;
    const GitRepo git(repo.dir());
    EXPECT_FALSE(git.resolveHead().has_value());
}

TEST(GitRepo, UnreadableHeadCommitIsAnError) {
    TempRepo repo;
    repo.write("a.txt", "a");
    const ObjectId commit = repo.commit();
    repo.removeObject(commit);
    EXPECT_THROW(GitRepo(repo.dir()).resolveHead(), GitError);
}

TEST(GitRepo, ResolvesHead) {
    TempRepo repo;
    repo.write("a.txt", "a");
    const ObjectId commit = repo.commit();
    EXPECT_EQ(GitRepo(repo.dir()).resolveHead(), commit);
}

TEST(GitRepo, ListsFilesWithKindsAndSizes) {
    TempRepo repo;
    repo.write("src/main.cpp", "int main() {}\n");
    repo.write("README", "hi");
    repo.symlink("link", "README");
    const ObjectId first = repo.commit();
    repo.addSubmodule("vendor/lib", first);
    const ObjectId commit = repo.commit();

    const std::vector<TreeEntry> entries = GitRepo(repo.dir()).listFiles(commit);
    ASSERT_EQ(entries.size(), 4U);  // sorted by path

    EXPECT_EQ(entries[0].path, "README");
    EXPECT_EQ(entries[0].kind, EntryKind::File);
    EXPECT_EQ(entries[0].size, 2U);

    EXPECT_EQ(entries[1].path, "link");
    EXPECT_EQ(entries[1].kind, EntryKind::Symlink);

    EXPECT_EQ(entries[2].path, "src/main.cpp");
    EXPECT_EQ(entries[2].kind, EntryKind::File);
    EXPECT_EQ(entries[2].size, 14U);

    EXPECT_EQ(entries[3].path, "vendor/lib");
    EXPECT_EQ(entries[3].kind, EntryKind::Submodule);
    EXPECT_EQ(entries[3].id, first);
}

TEST(GitRepo, ListsFilesSortedByPathBytes) {
    TempRepo repo;
    // Git sorts a folder as its name plus '/', so its tree order is the byte
    // order of the full paths. Doc IDs follow it, and search results are in
    // path order because of that.
    for (const std::string name : {"a0", "a/z", "a/b", "a.b", "a-b", "B", "\xC3\xA9"}) {
        repo.write(name, "x");
    }
    const ObjectId commit = repo.commit();
    std::vector<std::string> listed;
    for (const TreeEntry& entry : GitRepo(repo.dir()).listFiles(commit)) {
        listed.push_back(entry.path);
    }
    EXPECT_EQ(listed,
              (std::vector<std::string>{"B", "a-b", "a.b", "a/b", "a/z", "a0", "\xC3\xA9"}));
}

TEST(GitRepo, KeepsUnusualFileNames) {
    TempRepo repo;
    const std::vector<std::string> names = {
        "-leading-dash", "new\nline", "tab\there", "with space", "caf\xC3\xA9", "quote\"s",
    };
    for (const std::string& name : names) {
        repo.write(name, "x");
    }
    const ObjectId commit = repo.commit();

    std::vector<std::string> listed;
    for (const TreeEntry& entry : GitRepo(repo.dir()).listFiles(commit)) {
        listed.push_back(entry.path);
    }
    std::vector<std::string> expected = names;
    std::sort(expected.begin(), expected.end());
    std::sort(listed.begin(), listed.end());
    EXPECT_EQ(listed, expected);
}

TEST(BlobStream, ReadsExactBytes) {
    TempRepo repo;
    const std::string binary("a\0b\nc\r\n\xFF", 8);
    repo.write("bin", binary);
    repo.write("empty", "");
    repo.write("no-newline", "end");
    const ObjectId commit = repo.commit();

    const GitRepo git(repo.dir());
    const std::vector<TreeEntry> entries = git.listFiles(commit);
    const std::vector<std::string> expected = {binary, "", "end"};  // same order as the paths
    ASSERT_EQ(entries.size(), expected.size());

    BlobStream blobs(git);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Blob blob = blobs.read(entries[i].id, 1024);
        EXPECT_EQ(blob.status, BlobStatus::Found);
        EXPECT_EQ(blob.size, entries[i].size);
        EXPECT_EQ(blob.content, expected[i]);
    }
}

TEST(BlobStream, HandlesMissingAndTooLarge) {
    TempRepo repo;
    repo.write("big", std::string(5000, 'x'));
    repo.write("small", "small");
    const ObjectId commit = repo.commit();

    const GitRepo git(repo.dir());
    const std::vector<TreeEntry> entries = git.listFiles(commit);
    ASSERT_EQ(entries.size(), 2U);
    BlobStream blobs(git);

    const std::optional<ObjectId> unknownId = ObjectId::fromHex(kSampleHex);
    ASSERT_TRUE(unknownId.has_value());
    const Blob missing = blobs.read(unknownId.value(), 1024);
    EXPECT_EQ(missing.status, BlobStatus::Missing);

    const Blob big = blobs.read(entries[0].id, 1024);
    EXPECT_EQ(big.status, BlobStatus::TooLarge);
    EXPECT_EQ(big.size, 5000U);
    EXPECT_TRUE(big.content.empty());

    // The skipped bytes must not leak into the next reply.
    const Blob small = blobs.read(entries[1].id, 1024);
    EXPECT_EQ(small.status, BlobStatus::Found);
    EXPECT_EQ(small.content, "small");
}

TEST(FetchLatest, FollowsTheRemoteEvenAfterAForcePush) {
    TempRepo source;
    source.write("a.txt", "one");
    const ObjectId first = source.commit();
    const TempDir dir;
    const std::filesystem::path clone = dir.file("clone.git");
    cloneShallow("file://" + source.dir().string(), clone);
    EXPECT_EQ(GitRepo(clone).resolveHead(), first);

    source.write("a.txt", "two");
    const ObjectId second = source.commit();
    fetchLatest(clone);
    EXPECT_EQ(GitRepo(clone).resolveHead(), second);

    // Rewrite history, like a force-push: the new commit doesn't follow `second`.
    source.git({"reset", "--quiet", "--hard", first.hex()});
    source.write("a.txt", "three");
    const ObjectId rewritten = source.commit();
    fetchLatest(clone);
    EXPECT_EQ(GitRepo(clone).resolveHead(), rewritten);
}

TEST(FetchLatest, WorksWhenThePathHasANewline) {
    // A newline is allowed in a folder name, so Git's output can hold one
    // before the newline it adds at the end.
    TempRepo source;
    source.write("a.txt", "one");
    source.commit();
    const TempDir dir;
    const std::filesystem::path clone = dir.file("two\nlines") / "clone.git";
    cloneShallow("file://" + source.dir().string(), clone);

    source.write("a.txt", "two");
    const ObjectId second = source.commit();
    fetchLatest(clone);
    EXPECT_EQ(GitRepo(clone).resolveHead(), second);
}

TEST(FetchLatest, NeverTouchesARepoAroundTheFolder) {
    // `outer` has a remote, so a fetch run in it would work and move its
    // branch. A broken clone folder inside it must fail instead.
    TempRepo other;
    other.write("a.txt", "other");
    other.commit();
    TempRepo outer;
    outer.write("a.txt", "outer");
    const ObjectId head = outer.commit();
    outer.git({"remote", "add", "origin", other.dir().string()});
    std::filesystem::create_directories(outer.dir() / "clone.git");

    EXPECT_THROW(fetchLatest(outer.dir() / "clone.git"), GitError);
    EXPECT_EQ(GitRepo(outer.dir()).resolveHead(), head);
}

}  // namespace
}  // namespace codesearch
