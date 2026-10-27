// What our Git code does when Git fails or prints something broken. Each test
// swaps in a fake "git" script that prints exactly the output we want.

#include <gtest/gtest.h>

#include <string>

#include "../support/fake_git.hpp"
#include "codesearch/git/git_repo.hpp"

namespace codesearch {
namespace {

using testing::FakeGit;
using testing::kAnswerSha1;

const char* const kHex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

ObjectId sampleId() {
    std::optional<ObjectId> id = ObjectId::fromHex(kHex);
    if (!id.has_value()) {
        throw std::logic_error("bad sample ID");
    }
    return id.value();
}

// Expects `call` to throw GitError with `part` somewhere in the message.
template <typename Call>
void expectGitError(Call call, const std::string& part) {
    try {
        call();
        ADD_FAILURE() << "expected GitError containing \"" << part << "\"";
    } catch (const GitError& error) {
        EXPECT_NE(std::string(error.what()).find(part), std::string::npos) << error.what();
    }
}

// A fake ls-tree that prints `records` (with \000 between records).
std::string lsTreePrints(const std::string& records) {
    return std::string(kAnswerSha1) + "printf '" + records + "'\n";
}

// A fake cat-file that reads one request and answers with `reply`, a printf
// format where %s is the ID asked for.
std::string catFileReplies(const std::string& reply) {
    return std::string(kAnswerSha1) + "read id\nprintf '" + reply + "' \"$id\"\n";
}

std::vector<TreeEntry> listWith(const std::string& script) {
    const FakeGit git(script);
    return GitRepo(".").listFiles(sampleId());
}

Blob readWith(const std::string& script) {
    const FakeGit git(script);
    const GitRepo repo(".");
    BlobStream blobs(repo);
    return blobs.read(sampleId(), 1000);
}

TEST(GitErrors, RepoCheckFails) {
    expectGitError(
        [] {
            const FakeGit git("echo 'fatal: nope' >&2; exit 128\n");
            const GitRepo repo(".");
        },
        "not a Git repository");
    expectGitError(
        [] {
            const FakeGit git("echo sha1-ish\n");
            const GitRepo repo(".");
        },
        "sha1-ish");
}

TEST(GitErrors, HeadIsNotAnId) {
    const std::string script = std::string(kAnswerSha1) + "echo not-an-id\n";
    expectGitError(
        [&] {
            const FakeGit git(script);
            GitRepo(".").resolveHead();
        },
        "unexpected object ID");
}

TEST(GitErrors, HeadMissingButNotOnABranch) {
    // rev-parse HEAD fails and HEAD doesn't name a branch either.
    const std::string script = std::string(kAnswerSha1) + "exit 1\n";
    expectGitError(
        [&] {
            const FakeGit git(script);
            GitRepo(".").resolveHead();
        },
        "HEAD can't be read");
}

TEST(GitErrors, LsTreeFails) {
    const std::string script = std::string(kAnswerSha1) + "echo 'fatal: bad tree' >&2; exit 128\n";
    expectGitError(
        [&] {
            listWith(script);
        },
        "fatal: bad tree");
}

TEST(GitErrors, LsTreeRecordsAreBroken) {
    const std::string id = kHex;
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob " + id + " 5 no-tab\\000"));
        },
        "unexpected git ls-tree output");
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob " + id + "\\tpath\\000"));
        },
        "unexpected git ls-tree output");
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob " + id + " 5x\\tpath\\000"));
        },
        "unexpected size");
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob " + id + " -\\tpath\\000"));
        },
        "unexpected size");
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob " + id + " BAD\\tpath\\000"));
        },
        "missing the contents of path");
    expectGitError(
        [&] {
            listWith(lsTreePrints("100644 blob XYZ 5\\tpath\\000"));
        },
        "unexpected object ID");
}

TEST(GitErrors, LsTreeOutputWithoutFinalNulStillParses) {
    const std::vector<TreeEntry> entries =
        listWith(lsTreePrints("100644 blob " + std::string(kHex) + "      5\\tpath"));
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].path, "path");
    EXPECT_EQ(entries[0].size, 5U);
}

TEST(GitErrors, CatFileStopsWithoutAnswering) {
    const std::string script = std::string(kAnswerSha1) + "exit 0\n";
    expectGitError(
        [&] {
            readWith(script);
        },
        "stopped unexpectedly");
}

TEST(GitErrors, CatFileHeaderIsBroken) {
    expectGitError(
        [] {
            readWith(catFileReplies("bbbb blob 3\\nabc\\n"));
        },
        "unexpected reply");
    expectGitError(
        [] {
            readWith(catFileReplies("%s blob\\nabc\\n"));
        },
        "unexpected reply");
    expectGitError(
        [] {
            readWith(catFileReplies("%s blob 3x\\nabc\\n"));
        },
        "unexpected reply");
    expectGitError(
        [] {
            readWith(catFileReplies("%s tree 3\\nabc\\n"));
        },
        "not a file");
}

TEST(GitErrors, CatFileContentIsTruncated) {
    // Git says 10 bytes, then dies after 3.
    EXPECT_THROW(readWith(catFileReplies("%s blob 10\\nabc")), ProcessError);
    // A too-large blob gets skipped, which must fail the same way.
    EXPECT_THROW(readWith(catFileReplies("%s blob 5000\\nabc")), ProcessError);
}

TEST(GitErrors, CatFileContentHasNoFinalNewline) {
    expectGitError(
        [] {
            readWith(catFileReplies("%s blob 3\\nabcX"));
        },
        "unexpected reply");
}

TEST(GitErrors, CatFileHeaderEndsMidLine) {
    EXPECT_THROW(readWith(catFileReplies("%s blob 3")), ProcessError);
}

}  // namespace
}  // namespace codesearch
