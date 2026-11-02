#include "codesearch/repo/publisher.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/util/process.hpp"

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
    writeFile(workspace.tmpDir() / "r_00000001-g0003.idx", "half built");
    writeFile(workspace.tmpDir() / "r_00000001-g0003.idx.tmp-a1B2c3", "half written");

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
                                              workspace.reposDir() / "r_00000001.git");
    std::filesystem::create_directory_symlink(dir.file("precious"),
                                              workspace.tmpDir() / "r_00000001-g0001.idx");
    std::filesystem::create_symlink(dir.file("precious.idx"),
                                    workspace.indexesDir() / "r_00000001-g0001.idx");

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
              (std::set<std::string>{"code-search-workspace", "indexes", "notes.txt",
                                     "registry.json", "repos", "tmp"}));
    EXPECT_EQ(readFile(dir.file("outside.txt")), "keep me");
}

// Files code-search didn't make stay, even ones that look a bit like ours.
TEST(Publisher, CleanUpDeletesOnlyNamesWeMake) {
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    const std::set<std::string> strangers = {
        "notes.txt",
        "r_1.git",
        "r_0000000G.git",
        "r_00000001",
        "r_0000000g-g0001.idx",
        "r_00000001-g01.idx",
        "r_00000001-g0001.idx.bak",
        "x.tmp-a1B2c3",
    };
    for (const std::filesystem::path& folder :
         {workspace.tmpDir(), workspace.indexesDir(), workspace.reposDir()}) {
        for (const std::string& name : strangers) {
            writeFile(folder / name, "keep me");
        }
        std::filesystem::create_directories(folder / "project" / "src");
    }
    writeFile(workspace.tmpDir() / "r_00000001-g0001.idx", "ours");
    writeFile(workspace.indexesDir() / "r_00000001-g0001.idx", "ours");
    std::filesystem::create_directories(workspace.reposDir() / "r_00000001.git");

    cleanUp(workspace, Registry{});
    std::set<std::string> left = strangers;
    left.insert("project");
    for (const std::filesystem::path& folder :
         {workspace.tmpDir(), workspace.indexesDir(), workspace.reposDir()}) {
        EXPECT_EQ(namesIn(folder), left) << folder;
    }
}

// Pointing CODE_SEARCH_HOME at a folder that holds other files, like ~,
// must not turn that folder into a workspace.
TEST(Workspace, RefusesAFolderThatHoldsOtherFiles) {
    const TempDir dir;
    const Workspace home(dir.file("home"));
    std::filesystem::create_directories(home.tmpDir());
    writeFile(home.tmpDir() / "r_00000001-g0001.idx", "the user's file");
    EXPECT_THROW(home.create(), std::runtime_error);
    EXPECT_EQ(namesIn(home.root()), (std::set<std::string>{"tmp"}));  // nothing added

    writeFile(dir.file("file"), "x");
    EXPECT_THROW(Workspace(dir.file("file")).create(), std::runtime_error);
}

// Another program's registry.json doesn't make a folder ours, and nothing
// may be added to it.
TEST(Workspace, SomeoneElsesRegistryJsonIsntOurs) {
    const TempDir dir;
    const Workspace folder(dir.file("project"));
    std::filesystem::create_directories(folder.root());
    writeFile(folder.registryFile(), R"({"name":"my thing"})");
    writeFile(folder.root() / "notes.txt", "mine");
    EXPECT_THROW(folder.create(), std::runtime_error);
    EXPECT_EQ(namesIn(folder.root()), (std::set<std::string>{"notes.txt", "registry.json"}));
}

TEST(Workspace, TakesANewOrEmptyFolderOrAnOldWorkspace) {
    const TempDir dir;
    const Workspace fresh(dir.file("new"));
    fresh.create();
    EXPECT_TRUE(std::filesystem::exists(fresh.markerFile()));
    writeFile(fresh.tmpDir() / "r_00000001-g0001.idx", "x");
    EXPECT_NO_THROW(fresh.create());  // the marker says it's ours

    std::filesystem::create_directories(dir.file("empty"));
    EXPECT_NO_THROW(Workspace(dir.file("empty")).create());

    // Made before the marker existed: registry.json says it's ours.
    const Workspace old(dir.file("old"));
    std::filesystem::create_directories(old.indexesDir());
    writeFile(old.registryFile(), registryToJson(Registry{}));
    old.create();
    EXPECT_TRUE(std::filesystem::exists(old.markerFile()));
}

// Two commands run at once can both create the workspace. One may make the
// marker and folders while the other is checking the folder is empty;
// those are ours, so it must not refuse the folder.
TEST(Workspace, CommandsCanCreateItAtTheSameTime) {
    const TempDir dir;
    std::atomic<int> failures = 0;
    for (int round = 0; round < 200; ++round) {
        const Workspace workspace(dir.file("ws" + std::to_string(round)));
        std::filesystem::create_directories(workspace.root());
        std::vector<std::thread> commands;
        commands.reserve(4);
        for (int i = 0; i < 4; ++i) {
            commands.emplace_back([&] {
                try {
                    workspace.create();
                } catch (const std::runtime_error&) {
                    ++failures;
                }
            });
        }
        for (std::thread& command : commands) {
            command.join();
        }
    }
    EXPECT_EQ(failures, 0);
}

// A first add killed while it writes the marker must not lock the folder
// out. Now the marker is written in place, so a kill leaves it empty or cut
// short. An earlier version left a "code-search-workspace.tmp-XXXXXX" instead.
TEST(Workspace, RecoversFromAKilledFirstAdd) {
    const TempDir dir;
    const Workspace cutShort(dir.file("cut"));
    std::filesystem::create_directories(cutShort.root());
    writeFile(cutShort.markerFile(), "");
    EXPECT_NO_THROW(cutShort.create());

    const Workspace older(dir.file("older"));
    std::filesystem::create_directories(older.root());
    writeFile(older.root() / "code-search-workspace.tmp-a1B2c3", "half written");
    older.create();
    cleanUp(older, Registry{});
    EXPECT_EQ(namesIn(older.root()),
              (std::set<std::string>{"code-search-workspace", "indexes", "repos", "tmp"}));
}

// If the marker can't be made, no folders may be made either, or the next
// try would find a nonempty folder with no marker and refuse it for good.
TEST(Workspace, NoMarkerMeansNoFolders) {
#ifndef __APPLE__
    GTEST_SKIP() << "needs macOS ACLs to deny files but allow folders";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root ignores ACLs";
    }
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    std::filesystem::create_directories(workspace.root());
    // An ACL that refuses new files in the folder but still allows folders.
    const std::string rule = "everyone deny add_file";
    ASSERT_EQ(runProcess("chmod", {"+a", rule, workspace.root().string()}).exitCode, 0);
    EXPECT_THROW(workspace.create(), std::runtime_error);
    EXPECT_TRUE(namesIn(workspace.root()).empty());

    ASSERT_EQ(runProcess("chmod", {"-a", rule, workspace.root().string()}).exitCode, 0);
    workspace.create();  // works once files are allowed again
    EXPECT_TRUE(std::filesystem::exists(workspace.markerFile()));
#endif
}

// Something cleanup can't delete gives an error that says what and why.
TEST(Publisher, CleanUpSaysWhatItCouldntDelete) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can delete anything";
    }
    const TempDir dir;
    const Workspace workspace(dir.file("ws"));
    workspace.create();
    const std::filesystem::path locked = workspace.cloneDir("r_00000001") / "objects";
    std::filesystem::create_directories(locked);
    writeFile(locked / "pack", "x");
    std::filesystem::permissions(
        locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    try {
        cleanUp(workspace, Registry{});
        ADD_FAILURE() << "expected an error";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("could not delete"), std::string::npos) << message;
        EXPECT_NE(message.find("r_00000001.git"), std::string::npos) << message;
        EXPECT_NE(message.find("Check its permissions"), std::string::npos) << message;
    }
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
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
