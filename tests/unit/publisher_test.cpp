#include "codesearch/repo/publisher.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"
#include "codesearch/index/shard_reader.hpp"

namespace codesearch {
namespace {

using testing::readFile;
using testing::TempDir;
using testing::writeFile;

std::set<std::string> namesIn(const std::filesystem::path& dir) {
    std::set<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

RepoEntry repo(const std::string& id, RepoSource source) {
    return RepoEntry{
        .id = id,
        .name = id,
        .source = source,
        .location = "somewhere",
        .index = IndexInfo{.file = indexFileName(id, 2), .commit = testing::fakeObjectId('a')},
    };
}

TEST(Publisher, CleanUpKeepsOnlyWhatTheRegistryUses) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    Registry registry;
    registry.repos.push_back(repo("r_00000001", RepoSource::GitHub));
    registry.repos.push_back(repo("r_00000002", RepoSource::Local));

    writeFile(workspace.indexesDir() / "r_00000001-g0002.idx", "current");
    writeFile(workspace.indexesDir() / "r_00000001-g0001.idx", "old");
    writeFile(workspace.indexesDir() / "r_00000002-g0002.idx", "current");
    writeFile(workspace.indexesDir() / "r_00000003-g0001.idx", "removed repo");
    std::filesystem::create_directories(workspace.cloneDir("r_00000001") / "objects");
    std::filesystem::create_directories(workspace.cloneDir("r_00000002"));  // local: not ours
    std::filesystem::create_directories(workspace.cloneDir("r_00000003") / "objects");
    writeFile(workspace.tmpDir() / "half-built.idx", "x");
    std::filesystem::create_directories(workspace.tmpDir() / "folder" / "inside");

    cleanUp(workspace, registry);
    EXPECT_EQ(namesIn(workspace.indexesDir()),
              (std::set<std::string>{"r_00000001-g0002.idx", "r_00000002-g0002.idx"}));
    EXPECT_EQ(namesIn(workspace.reposDir()), (std::set<std::string>{"r_00000001.git"}));
    EXPECT_TRUE(namesIn(workspace.tmpDir()).empty());
}

TEST(Publisher, CleanUpNeverFollowsSymlinksOut) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    std::filesystem::create_directories(dir.file("precious"));
    writeFile(dir.file("precious") / "code.txt", "keep me");
    writeFile(dir.file("precious.idx"), "keep me too");
    std::filesystem::create_directory_symlink(dir.file("precious"),
                                              workspace.reposDir() / "r_x.git");
    std::filesystem::create_directory_symlink(dir.file("precious"), workspace.tmpDir() / "link");
    std::filesystem::create_symlink(dir.file("precious.idx"), workspace.indexesDir() / "a.idx");

    cleanUp(workspace, Registry{});
    EXPECT_TRUE(namesIn(workspace.reposDir()).empty());  // the links are gone...
    EXPECT_TRUE(namesIn(workspace.tmpDir()).empty());
    EXPECT_TRUE(namesIn(workspace.indexesDir()).empty());
    EXPECT_EQ(readFile(dir.file("precious") / "code.txt"),
              "keep me");  // ...not what they pointed to
    EXPECT_EQ(readFile(dir.file("precious.idx")), "keep me too");
}

// tmp/, indexes/ or repos/ replaced by a symlink to a folder elsewhere.
// Cleanup must refuse, not delete what's in the other folder.
TEST(Publisher, CleanUpRefusesSymlinkedFolders) {
    for (const std::string name : {"tmp", "indexes", "repos"}) {
        SCOPED_TRACE(name);
        const TempDir dir;
        const Workspace workspace(dir.file("ws"));
        workspace.create();
        std::filesystem::create_directories(dir.file("elsewhere"));
        writeFile(dir.file("elsewhere") / "precious.txt", "keep me");
        std::filesystem::remove(workspace.root() / name);
        std::filesystem::create_directory_symlink(dir.file("elsewhere"), workspace.root() / name);

        EXPECT_THROW(cleanUp(workspace, Registry{}), std::runtime_error);
        EXPECT_THROW(workspace.create(), std::runtime_error);
        EXPECT_EQ(readFile(dir.file("elsewhere") / "precious.txt"), "keep me");
    }
}

TEST(Publisher, CleanUpRefusesAFileInsteadOfAFolder) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    std::filesystem::remove(workspace.tmpDir());
    writeFile(workspace.tmpDir(), "not a folder");
    EXPECT_THROW(cleanUp(workspace, Registry{}), std::runtime_error);
}

// A registry save killed partway leaves registry.json.tmp-XXXXXX next to it.
TEST(Publisher, CleanUpRemovesRegistryTempFiles) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    writeFile(workspace.registryFile(), "{}");
    writeFile(workspace.root() / "registry.json.tmp-a1B2c3", "half written");
    writeFile(workspace.root() / "notes.txt", "not ours to delete");
    writeFile(dir.file("outside.txt"), "keep me");
    std::filesystem::create_symlink(dir.file("outside.txt"),
                                    workspace.root() / "registry.json.tmp-link");

    cleanUp(workspace, Registry{});
    EXPECT_EQ(namesIn(workspace.root()),
              (std::set<std::string>{"indexes", "notes.txt", "registry.json", "repos", "tmp"}));
    EXPECT_EQ(readFile(dir.file("outside.txt")), "keep me");
}

TEST(Publisher, CleanUpWithoutFoldersDoesNothing) {
    const TempDir dir;
    EXPECT_NO_THROW(cleanUp(Workspace(dir.file("ws")), Registry{}));
}

TEST(Publisher, WriteThenMove) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    IndexBuilder builder;
    builder.add("a.txt", "hello\n");
    const MemoryIndex index = builder.build();
    const ShardInfo info{.commit = testing::fakeObjectId('b')};

    const std::filesystem::path tmpFile =
        writeToTmp(workspace, index, info, "r_00000001-g0001.idx");
    EXPECT_EQ(tmpFile, workspace.tmpDir() / "r_00000001-g0001.idx");
    moveIntoIndexes(workspace, tmpFile);
    EXPECT_TRUE(namesIn(workspace.tmpDir()).empty());
    const ShardReader reader(workspace.indexesDir() / "r_00000001-g0001.idx");
    EXPECT_EQ(reader.commit(), info.commit);
    EXPECT_EQ(reader.docCount(), 1U);
}

}  // namespace
}  // namespace codesearch
