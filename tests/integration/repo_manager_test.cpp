// Adding, listing, searching, updating and removing repos in a workspace, start to
// finish, including what happens when a command is killed partway.

#include "codesearch/app/repo_manager.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/temp_repo.hpp"
#include "codesearch/git/git_repo.hpp"
#include "codesearch/git/git_runner.hpp"
#include "codesearch/repo/publisher.hpp"
#include "codesearch/search/searcher.hpp"
#include "codesearch/util/atomic_file.hpp"
#include "codesearch/util/file_lock.hpp"

namespace codesearch {
namespace {

using testing::readFile;
using testing::TempDir;
using testing::TempRepo;
using testing::writeFile;

// A "GitHub" clone that copies a local repo instead, so tests don't need
// the network. It checks the URL it's given is the one we expect.
Cloner fakeGitHub(const TempRepo& source) {
    return [&source](const std::string& url, const std::filesystem::path& dest) {
        EXPECT_EQ(url, "https://github.com/example/frontend");
        const ProcessResult result = runProcess(
            "git", {"clone", "--bare", "--quiet", "--", source.dir().string(), dest.string()});
        if (result.exitCode != 0) {
            throw GitError("clone failed: " + result.err);
        }
    };
}

Cloner noGitHub() {
    return [](const std::string&, const std::filesystem::path&) {
        throw std::logic_error("this test shouldn't clone");
    };
}

std::set<std::string> namesIn(const std::filesystem::path& dir) {
    std::set<std::string> names;
    if (std::filesystem::exists(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            names.insert(entry.path().filename().string());
        }
    }
    return names;
}

// The paths of files matching `text` in each repo, like "backend:a.txt".
std::vector<std::string> searchAll(const Workspace& workspace, const std::string& text,
                                   const std::string& onlyName = "") {
    std::vector<std::string> found;
    const QueryNode query =
        onlyName.empty() ? literal(text) : allOf({repoFilter(onlyName), literal(text)});
    for (const OpenRepo& repo : openRepos(workspace, query)) {
        for (const FileMatch& file : search(*repo.index, literal(text)).files) {
            found.push_back(repo.name + ":" + file.path);
        }
    }
    return found;
}

// Checks the workspace holds exactly what the registry uses, and nothing else.
void expectTidy(const Workspace& workspace) {
    const Registry registry = loadRegistry(workspace);
    std::set<std::string> indexes;
    std::set<std::string> clones;
    for (const RepoEntry& repo : registry.repos) {
        indexes.insert(repo.index.file);
        if (repo.source == RepoSource::GitHub) {
            clones.insert(repo.id + ".git");
        }
    }
    EXPECT_EQ(namesIn(workspace.indexesDir()), indexes);
    EXPECT_EQ(namesIn(workspace.reposDir()), clones);
    EXPECT_TRUE(namesIn(workspace.tmpDir()).empty());
}

// Runs `fn`, expecting a std::runtime_error whose message contains `part`.
template <typename Fn>
void expectError(Fn fn, const std::string& part) {
    try {
        fn();
        ADD_FAILURE() << "expected an error containing: " << part;
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(part), std::string::npos)
            << "got: " << error.what() << "\nwanted: " << part;
    }
}

void fillBackend(TempRepo& repo) {
    repo.write("a.txt", "shared needle\n");
    repo.write("b.txt", "backend only\n");
    repo.commit();
}

ObjectId fillFrontend(TempRepo& repo) {
    repo.write("app.js", "shared needle in the frontend\n");
    return repo.commit();
}

TEST(RepoManager, AddSearchListRemove) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    TempRepo frontend;
    fillFrontend(frontend);
    RepoManager manager(workspace, fakeGitHub(frontend));

    const AddResult added = manager.add(backend.dir().string(), "backend");
    EXPECT_EQ(added.repo.source, RepoSource::Local);
    EXPECT_EQ(added.repo.location, std::filesystem::canonical(backend.dir()).string());
    EXPECT_EQ(added.report.indexed.files, 2U);
    manager.add("https://github.com/example/frontend.git", "");  // name from the URL
    // GitHub names don't depend on case, so this is the same repo.
    EXPECT_THROW(manager.add("https://github.com/Example/FrontEnd", "again"), std::runtime_error);

    const Registry registry = loadRegistry(workspace);
    ASSERT_EQ(registry.repos.size(), 2U);
    EXPECT_EQ(registry.repos[0].name, "backend");
    EXPECT_EQ(registry.repos[1].name, "frontend");
    EXPECT_EQ(registry.repos[1].location, "https://github.com/example/frontend");
    expectTidy(workspace);

    EXPECT_EQ(searchAll(workspace, "needle"),
              (std::vector<std::string>{"backend:a.txt", "frontend:app.js"}));
    EXPECT_EQ(searchAll(workspace, "needle", "frontend"),
              (std::vector<std::string>{"frontend:app.js"}));
    expectError(
        [&] {
            searchAll(workspace, "needle", "nope");
        },
        "no repo named nope");

    manager.remove("frontend");
    EXPECT_EQ(searchAll(workspace, "needle"), (std::vector<std::string>{"backend:a.txt"}));
    expectTidy(workspace);  // the clone is gone too
    expectError(
        [&] {
            manager.remove("frontend");
        },
        "no repo named frontend");
}

TEST(RepoManager, DefaultNameIsTheFolderName) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    const AddResult added = RepoManager(workspace, noGitHub()).add(backend.dir().string(), "");
    EXPECT_EQ(added.repo.name, std::filesystem::canonical(backend.dir()).filename().string());
}

TEST(RepoManager, RemoveLeavesLocalCodeAlone) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    backend.writeUnstaged("notes.txt", "uncommitted work\n");
    RepoManager manager(workspace, noGitHub());

    manager.add(backend.dir().string(), "backend");
    manager.remove("backend");
    EXPECT_EQ(readFile(backend.dir() / "a.txt"), "shared needle\n");
    EXPECT_EQ(readFile(backend.dir() / "notes.txt"), "uncommitted work\n");
    EXPECT_EQ(backend.git({"status", "--porcelain"}).out, "?? notes.txt\n");
    EXPECT_TRUE(loadRegistry(workspace).repos.empty());
    expectTidy(workspace);
}

TEST(RepoManager, BadAddsChangeNothing) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    const TempRepo empty;
    const TempDir notARepo;
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");
    const std::string before = readFile(workspace.registryFile());

    expectError(
        [&] {
            manager.add(notARepo.dir().string(), "x");
        },
        "not a Git repository");
    expectError(
        [&] {
            manager.add(empty.dir().string(), "x");
        },
        "has no commits yet");
    expectError(
        [&] {
            manager.add(home.file("missing").string(), "x");
        },
        "no such folder");
    expectError(
        [&] {
            manager.add(backend.dir().string(), "again");
        },
        "already added as backend");
    expectError(
        [&] {
            manager.add(empty.dir().string(), "backend");
        },
        "already exists");
    expectError(
        [&] {
            manager.add(empty.dir().string(), "bad name");
        },
        "repo names can only use");
    expectError(
        [&] {
            manager.add(workspace.root().string(), "x");
        },
        "data folder");
    expectError(
        [&] {
            manager.add(workspace.reposDir().string(), "x");
        },
        "data folder");
    expectError(
        [&] {
            manager.add("https://gitlab.com/a/b", "x");
        },
        "only https://github.com/");
    expectError(
        [&] {
            manager.add("git@github.com:a/b.git", "x");
        },
        "only https://github.com/");

    EXPECT_EQ(readFile(workspace.registryFile()), before);
    expectTidy(workspace);
}

// Git finds a repo from any folder inside it, so only the top folder of a
// repo can be added. Otherwise a subfolder would index the whole repo, and
// the same repo could be added twice under two names.
TEST(RepoManager, AddsOnlyTheTopFolderOfARepo) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    backend.write("src/c.txt", "in a subfolder\n");
    fillBackend(backend);
    RepoManager manager(workspace, noGitHub());
    const std::string top = std::filesystem::canonical(backend.dir()).string();
    expectError(
        [&] {
            manager.add((backend.dir() / "src").string(), "x");
        },
        "is inside the repo " + top + ". Add " + top + " instead");
    for (const std::string inside : {".git", ".git/refs"}) {
        expectError(
            [&] {
                manager.add((backend.dir() / inside).string(), "x");
            },
            "is inside a repo's .git folder");
    }
    EXPECT_TRUE(loadRegistry(workspace).repos.empty());

    // A bare repo is its own top folder.
    const std::filesystem::path bare = home.file("bare.git");
    ASSERT_EQ(runProcess("git", {"clone", "--quiet", "--bare", "--", backend.dir().string(),
                                 bare.string()})
                  .exitCode,
              0);
    expectError(
        [&] {
            manager.add((bare / "refs").string(), "x");
        },
        "is inside the repo");
    EXPECT_EQ(manager.add(bare.string(), "bare").repo.name, "bare");
    EXPECT_EQ(manager.add(backend.dir().string(), "backend").repo.name, "backend");
}

TEST(RepoManager, FailedCloneLeavesNothing) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    RepoManager manager(workspace, [](const std::string&, const std::filesystem::path& dest) {
        std::filesystem::create_directories(dest / "objects");  // a half-made clone
        throw GitError("could not clone: network is down");
    });
    expectError(
        [&] {
            manager.add("https://github.com/example/frontend", "");
        },
        "network is down");
    EXPECT_TRUE(loadRegistry(workspace).repos.empty());
    expectTidy(workspace);
}

TEST(RepoManager, SecondCommandWaitsForTheLock) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    workspace.create();

    // Pretend another command is building: it holds the lock and has a file in tmp/.
    std::optional<FileLock> otherCommand;
    otherCommand.emplace(workspace.lockFile());
    writeFile(workspace.tmpDir() / "r_00000001-g0001.idx", "in progress");

    std::atomic<bool> waited = false;
    std::future<void> adding = std::async(std::launch::async, [&] {
        RepoManager manager(workspace, noGitHub());
        manager.setOnWait([&] {
            waited = true;
        });
        manager.add(backend.dir().string(), "backend");
    });
    EXPECT_EQ(adding.wait_for(std::chrono::milliseconds(500)), std::future_status::timeout);
    EXPECT_TRUE(std::filesystem::exists(workspace.tmpDir() / "r_00000001-g0001.idx"));
    EXPECT_TRUE(loadRegistry(workspace).repos.empty());

    otherCommand.reset();  // the other command finishes
    ASSERT_EQ(adding.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    adding.get();
    EXPECT_TRUE(waited);
    EXPECT_EQ(loadRegistry(workspace).repos.size(), 1U);
    expectTidy(workspace);  // its leftovers are cleaned up now that it's done
}

TEST(RepoManager, SearchesWorkDuringAChange) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");

    // Searching doesn't take the lock, so it works while a change holds it.
    {
        const FileLock otherCommand(workspace.lockFile());
        EXPECT_EQ(searchAll(workspace, "needle"), (std::vector<std::string>{"backend:a.txt"}));
    }

    // An index that's already open stays readable after remove deletes it.
    std::vector<OpenRepo> open = openRepos(workspace, literal("needle"));
    manager.remove("backend");
    EXPECT_TRUE(namesIn(workspace.indexesDir()).empty());  // the file is deleted
    ASSERT_EQ(open.size(), 1U);
    EXPECT_EQ(search(*open[0].index, literal("needle")).files.size(), 1U);
}

TEST(RepoManager, MissingIndexFileIsAnError) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    RepoManager(workspace, noGitHub()).add(backend.dir().string(), "backend");
    std::filesystem::remove(workspace.indexesDir() / loadRegistry(workspace).repos[0].index.file);
    expectError(
        [&] {
            openRepos(workspace, literal("x"));
        },
        "could not open");
}

// ---- Update ----

// The index file name the registry has for repo `name`.
std::string indexFileOf(const Workspace& workspace, const std::string& name) {
    const Registry registry = loadRegistry(workspace);  // `repo` points into this
    const RepoEntry* repo = registry.find(name);
    return repo == nullptr ? "" : repo->index.file;
}

TEST(RepoManager, UpdatePicksUpNewCommits) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    RepoManager manager(workspace, noGitHub());
    const std::string id = manager.add(backend.dir().string(), "backend").repo.id;
    EXPECT_EQ(indexFileOf(workspace, "backend"), id + "-g0001.idx");

    // Nothing changed: nothing happens.
    const std::string before = readFile(workspace.registryFile());
    UpdateResult result = manager.update("backend", false);
    EXPECT_EQ(result.reason, "");
    EXPECT_EQ(readFile(workspace.registryFile()), before);
    EXPECT_EQ(namesIn(workspace.indexesDir()), (std::set<std::string>{id + "-g0001.idx"}));

    backend.write("c.txt", "fresh needle\n");
    const ObjectId commit = backend.commit();
    result = manager.update("backend", false);
    EXPECT_EQ(result.reason, "new commit");
    EXPECT_EQ(result.repo.index.commit, commit);
    EXPECT_EQ(result.report.indexed.files, 3U);
    EXPECT_EQ(indexFileOf(workspace, "backend"), id + "-g0002.idx");
    EXPECT_EQ(searchAll(workspace, "needle"),
              (std::vector<std::string>{"backend:a.txt", "backend:c.txt"}));
    expectTidy(workspace);  // the old index file is gone

    // --rebuild builds even when nothing changed.
    result = manager.update("backend", true);
    EXPECT_EQ(result.reason, "asked to rebuild");
    EXPECT_EQ(indexFileOf(workspace, "backend"), id + "-g0003.idx");
    expectTidy(workspace);

    expectError(
        [&] {
            manager.update("nope", false);
        },
        "no repo named nope");
}

TEST(RepoManager, UpdateFetchesGitHubRepos) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo frontend;
    const ObjectId first = fillFrontend(frontend);
    RepoManager manager(workspace, fakeGitHub(frontend));
    manager.add("https://github.com/example/frontend", "");
    EXPECT_EQ(manager.update("frontend", false).reason, "");

    frontend.write("new.js", "needle again\n");
    const ObjectId second = frontend.commit();
    UpdateResult result = manager.update("frontend", false);
    EXPECT_EQ(result.reason, "new commit");
    EXPECT_EQ(result.repo.index.commit, second);
    EXPECT_EQ(searchAll(workspace, "needle"),
              (std::vector<std::string>{"frontend:app.js", "frontend:new.js"}));

    // A force-push that drops new.js again.
    frontend.git({"reset", "--quiet", "--hard", first.hex()});
    frontend.write("other.js", "rewritten\n");
    const ObjectId rewritten = frontend.commit();
    result = manager.update("frontend", false);
    EXPECT_EQ(result.repo.index.commit, rewritten);
    EXPECT_EQ(searchAll(workspace, "needle"), (std::vector<std::string>{"frontend:app.js"}));
    expectTidy(workspace);
}

TEST(RepoManager, FailedUpdateKeepsTheOldIndex) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    std::optional<TempRepo> frontend;
    frontend.emplace();
    fillFrontend(*frontend);
    RepoManager manager(workspace, fakeGitHub(*frontend));
    manager.add(backend.dir().string(), "backend");
    manager.add("https://github.com/example/frontend", "");
    const std::string before = readFile(workspace.registryFile());

    frontend.reset();  // the remote is gone, so fetching fails
    expectError(
        [&] {
            manager.update("frontend", false);
        },
        "could not fetch");

    // A failure after the new file is made: it gets deleted right away.
    for (const std::string step : {"written", "moved"}) {
        SCOPED_TRACE("failed after " + step);
        manager.setOnStep([&](std::string_view now) {
            if (now == step) {
                throw std::runtime_error("disk full");
            }
        });
        expectError(
            [&] {
                manager.update("backend", true);
            },
            "disk full");
        expectTidy(workspace);
    }
    manager.setOnStep({});

    // A local repo with a file Git can't read: the build fails partway.
    backend.write("c.txt", "will go missing\n");
    backend.commit();
    backend.removeObject(backend.blobId("c.txt"));
    expectError(
        [&] {
            manager.update("backend", false);
        },
        "c.txt");

    EXPECT_EQ(readFile(workspace.registryFile()), before);
    EXPECT_EQ(searchAll(workspace, "needle"),
              (std::vector<std::string>{"backend:a.txt", "frontend:app.js"}));
    expectTidy(workspace);
}

// Each way of pointing the clone at another repo makes update refuse, and
// that repo's branch stays where it was.
TEST(RepoManager, UpdateOnlyWritesToItsOwnClone) {
    TempRepo frontend;
    fillFrontend(frontend);
    // A repo of the user's. It has the same remote, so a fetch in it would
    // work and move its branch.
    TempRepo victim;
    victim.write("mine.txt", "mine\n");
    const ObjectId victimHead = victim.commit();
    victim.git({"remote", "add", "origin", frontend.dir().string()});

    struct Case {
        std::string name;
        std::function<void(const std::filesystem::path& clone)> redirect;
        std::string error;
    };
    const std::vector<Case> cases = {
        {
            .name = "the clone is a symlink",
            .redirect =
                [&](const std::filesystem::path& clone) {
                    std::filesystem::remove_all(clone);
                    std::filesystem::create_directory_symlink(victim.dir() / ".git", clone);
                },
            .error = "must be a plain folder",
        },
        {
            .name = "refs/ inside the clone is a symlink",
            .redirect =
                [&](const std::filesystem::path& clone) {
                    std::filesystem::remove_all(clone / "refs");
                    std::filesystem::create_directory_symlink(victim.dir() / ".git" / "refs",
                                                              clone / "refs");
                },
            .error = "refs is a symlink",
        },
        {
            .name = "a commondir file",
            .redirect =
                [&](const std::filesystem::path& clone) {
                    writeFile(clone / "commondir", (victim.dir() / ".git").string() + "\n");
                },
            .error = "Git would write to another folder",
        },
        {
            .name = "the clone is missing",
            .redirect =
                [&](const std::filesystem::path& clone) {
                    std::filesystem::remove_all(clone);
                },
            .error = "is missing",
        },
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const TempDir home;
        const Workspace workspace(home.file("ws"));
        RepoManager manager(workspace, fakeGitHub(frontend));
        const std::string id = manager.add("https://github.com/example/frontend", "").repo.id;
        frontend.write("new.js", "new\n");
        frontend.commit();

        c.redirect(workspace.cloneDir(id));
        expectError(
            [&] {
                manager.update("frontend", false);
            },
            c.error);
        EXPECT_EQ(GitRepo(victim.dir()).resolveHead(), victimHead);
        EXPECT_EQ(victim.git({"status", "--porcelain"}).out, "");
    }
}

TEST(RepoManager, ChangedSettingsCauseARebuild) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    backend.write("small.txt", "needle\n");
    backend.write("big.txt", "needle " + std::string(100, 'x') + "\n");
    backend.commit();
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");

    FilterConfig config;
    config.maxFileBytes = 50;
    manager.setFilterConfig(config);
    const UpdateResult result = manager.update("backend", false);
    EXPECT_EQ(result.reason, "changed settings");
    EXPECT_EQ(result.report.skipped.at(SkipReason::TooLarge).files, 1U);
    EXPECT_EQ(searchAll(workspace, "needle"), (std::vector<std::string>{"backend:small.txt"}));
    EXPECT_EQ(manager.update("backend", false).reason, "");  // and then it's current
}

TEST(RepoManager, OlderFormatOrRulesCauseARebuild) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");

    // Each time, pretend the index was built by an older code-search, by
    // changing what the registry says about it.
    JsonRegistryStore store(workspace.registryFile());
    Registry registry = store.load();
    registry.repos[0].index.formatVersion = 0;
    store.save(registry);
    EXPECT_EQ(manager.update("backend", false).reason, "new index format");

    registry = store.load();
    registry.repos[0].index.policyVersion = kPolicyVersion - 1;
    store.save(registry);
    EXPECT_EQ(manager.update("backend", false).reason, "new filter rules");

    registry = store.load();
    registry.repos[0].index.configFingerprint += 1;
    store.save(registry);
    EXPECT_EQ(manager.update("backend", false).reason, "changed settings");

    EXPECT_EQ(manager.update("backend", false).reason, "");
    expectTidy(workspace);
}

// Runs update and checks it rebuilt the index for a reason containing `part`.
void expectRebuilt(RepoManager& manager, const Workspace& workspace, const std::string& part) {
    const std::string reason = manager.update("backend", false).reason;
    EXPECT_NE(reason.find(part), std::string::npos) << reason;
    EXPECT_EQ(searchAll(workspace, "needle", "backend"),
              (std::vector<std::string>{"backend:a.txt"}));
    expectTidy(workspace);
}

TEST(RepoManager, BadIndexFilesGetRebuilt) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    TempRepo other;
    other.write("x.txt", "other\n");
    other.commit();
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");
    manager.add(other.dir().string(), "other");
    const auto backendFile = [&] {
        return workspace.indexesDir() / indexFileOf(workspace, "backend");
    };

    std::filesystem::remove(backendFile());
    expectRebuilt(manager, workspace, "can't be used: could not open");

    writeFile(backendFile(), "hello");
    expectRebuilt(manager, workspace, "can't be used: not an index file");

    // A good index file, but of another commit than the registry says.
    writeFile(backendFile(), readFile(workspace.indexesDir() / indexFileOf(workspace, "other")));
    expectRebuilt(manager, workspace, "doesn't match the registry");
}

TEST(RepoManager, OpenReposOpensOnlyWhatTheQueryNeeds) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    TempRepo other;
    other.write("x.txt", "needle\n");
    other.commit();
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");
    manager.add(other.dir().string(), "other");

    // A damaged backend index doesn't get in the way of searching other.
    writeFile(workspace.indexesDir() / indexFileOf(workspace, "backend"), "damaged");
    const std::vector<OpenRepo> opened =
        openRepos(workspace, allOf({repoFilter("other"), literal("needle")}));
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(opened[0].name, "other");
    EXPECT_EQ(openRepos(workspace, negate(repoFilter("backend"))).size(), 1U);
    EXPECT_EQ(
        openRepos(workspace, negate(anyOf({repoFilter("backend"), literal("missing")}))).size(),
        1U);
    expectError(
        [&] {
            openRepos(workspace, literal("needle"));
        },
        "backend: not an index file");
    expectError(
        [&] {
            openRepos(workspace, anyOf({repoFilter("other"), repoFilter("typo")}));
        },
        "no repo named typo");
}

// ---- Killed partway ----

// Runs `fn` in a child process (fork) and returns its exit code. The child
// is a copy of this test process; `fn` ends it with _exit, which stops it on
// the spot like a kill, without running destructors or cleanup.
int runInChild(const std::function<void()>& fn) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        try {
            fn();
        } catch (...) {
            ::_exit(3);
        }
        ::_exit(4);  // finished without being stopped
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Runs `change` in a child process that dies right after step `stopAt`.
// Returns true if it got there.
bool stopAfterStep(const Workspace& workspace, const Cloner& cloner, const std::string& stopAt,
                   const std::function<void(RepoManager&)>& change) {
    const int exitCode = runInChild([&] {
        RepoManager manager(workspace, cloner);
        manager.setOnStep([&](std::string_view step) {
            if (step == stopAt) {
                ::_exit(0);
            }
        });
        change(manager);
    });
    return exitCode == 0;
}

TEST(RepoManager, SymlinkedFolderStopsAdd) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    workspace.create();
    std::filesystem::create_directories(home.file("elsewhere"));
    writeFile(home.file("elsewhere") / "precious.txt", "keep me");
    std::filesystem::remove(workspace.tmpDir());
    std::filesystem::create_directory_symlink(home.file("elsewhere"), workspace.tmpDir());

    expectError(
        [&] {
            RepoManager(workspace, noGitHub()).add(backend.dir().string(), "x");
        },
        "must be a plain folder");
    EXPECT_EQ(namesIn(home.file("elsewhere")), (std::set<std::string>{"precious.txt"}));
}

// Cleanup deletes the index files and clones the registry doesn't list, so
// without registry.json it would delete them all. Changes refuse instead.
// Files cleanup never deletes, like .DS_Store, don't block the first add.
TEST(RepoManager, OtherFilesDontCountAsIndexes) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    workspace.create();
    writeFile(workspace.indexesDir() / ".DS_Store", "x");
    TempRepo backend;
    fillBackend(backend);
    EXPECT_NO_THROW(RepoManager(workspace, noGitHub()).add(backend.dir().string(), "backend"));
}

TEST(RepoManager, MissingRegistryDeletesNothing) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    TempRepo other;
    other.write("x.txt", "x\n");
    other.commit();
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");
    const std::string indexFile = loadRegistry(workspace).repos[0].index.file;
    std::filesystem::remove(workspace.registryFile());

    expectError(
        [&] {
            manager.add(other.dir().string(), "other");
        },
        "registry.json, which lists the added repos, is missing");
    expectError(
        [&] {
            manager.remove("backend");
        },
        "registry.json, which lists the added repos, is missing");
    // list and search say so too, instead of "nothing added yet".
    EXPECT_THROW(loadRegistry(workspace), std::runtime_error);
    EXPECT_TRUE(std::filesystem::exists(workspace.indexesDir() / indexFile));
    EXPECT_FALSE(std::filesystem::exists(workspace.registryFile()));
}

TEST(RepoManager, KilledRegistrySaveIsCleanedUp) {
    const TempDir home;
    const Workspace workspace(home.file("ws"));
    TempRepo backend;
    fillBackend(backend);
    RepoManager manager(workspace, noGitHub());
    manager.add(backend.dir().string(), "backend");
    const std::string before = readFile(workspace.registryFile());

    // A save that dies after writing part of the new registry.
    EXPECT_EQ(runInChild([&] {
                  AtomicFile file(workspace.registryFile());
                  file.write("{\"version\": 1, \"rep");
                  ::_exit(0);
              }),
              0);
    EXPECT_EQ(readFile(workspace.registryFile()), before);  // the old one is intact
    EXPECT_EQ(namesIn(workspace.root()).size(), 7U);        // with the leftover

    manager.remove("backend");  // any change cleans up first
    EXPECT_EQ(namesIn(workspace.root()),
              (std::set<std::string>{"code-search-workspace", "indexes", "lock", "registry.json",
                                     "repos", "tmp"}));
}

TEST(RepoManager, KilledAddLeavesACompleteState) {
    TempRepo backend;
    fillBackend(backend);
    TempRepo frontend;
    fillFrontend(frontend);

    for (const std::string step : {"locked", "cloned", "written", "moved", "saved", "cleaned"}) {
        SCOPED_TRACE("killed after " + step);  // added to any failure message below
        const TempDir home;
        const Workspace workspace(home.file("ws"));
        RepoManager(workspace, noGitHub()).add(backend.dir().string(), "backend");

        ASSERT_TRUE(stopAfterStep(workspace, fakeGitHub(frontend), step, [](RepoManager& m) {
            m.add("https://github.com/example/frontend", "");
        }));

        // The registry is complete, and every index it names opens and searches.
        const bool saved = step == "saved" || step == "cleaned";
        const std::vector<std::string> expected =
            saved ? std::vector<std::string>{"backend:a.txt", "frontend:app.js"}
                  : std::vector<std::string>{"backend:a.txt"};
        EXPECT_EQ(searchAll(workspace, "needle"), expected);

        // The next command cleans up whatever was left.
        cleanUp(workspace, loadRegistry(workspace));
        expectTidy(workspace);
        EXPECT_EQ(searchAll(workspace, "needle"), expected);
    }
}

TEST(RepoManager, KilledRemoveLeavesACompleteState) {
    TempRepo backend;
    fillBackend(backend);
    TempRepo frontend;
    fillFrontend(frontend);

    for (const std::string step : {"locked", "saved", "cleaned"}) {
        SCOPED_TRACE("killed after " + step);
        const TempDir home;
        const Workspace workspace(home.file("ws"));
        RepoManager setup(workspace, fakeGitHub(frontend));
        setup.add(backend.dir().string(), "backend");
        setup.add("https://github.com/example/frontend", "");

        ASSERT_TRUE(stopAfterStep(workspace, noGitHub(), step, [](RepoManager& m) {
            m.remove("frontend");
        }));

        const std::vector<std::string> expected =
            step == "locked" ? std::vector<std::string>{"backend:a.txt", "frontend:app.js"}
                             : std::vector<std::string>{"backend:a.txt"};
        EXPECT_EQ(searchAll(workspace, "needle"), expected);
        cleanUp(workspace, loadRegistry(workspace));
        expectTidy(workspace);
    }
}

TEST(RepoManager, KilledUpdateKeepsAUsableIndex) {
    TempRepo frontend;
    fillFrontend(frontend);

    for (const std::string step : {"locked", "fetched", "written", "moved", "saved", "cleaned"}) {
        SCOPED_TRACE("killed after " + step);
        const TempDir home;
        const Workspace workspace(home.file("ws"));
        TempRepo source;
        fillFrontend(source);
        RepoManager(workspace, fakeGitHub(source)).add("https://github.com/example/frontend", "");
        source.write("new.js", "needle too\n");
        source.commit();

        ASSERT_TRUE(stopAfterStep(workspace, noGitHub(), step, [](RepoManager& m) {
            m.update("frontend", false);
        }));

        const bool saved = step == "saved" || step == "cleaned";
        const std::vector<std::string> expected =
            saved ? std::vector<std::string>{"frontend:app.js", "frontend:new.js"}
                  : std::vector<std::string>{"frontend:app.js"};
        EXPECT_EQ(searchAll(workspace, "needle"), expected);
        cleanUp(workspace, loadRegistry(workspace));
        expectTidy(workspace);

        // The next update finishes the job.
        RepoManager(workspace, noGitHub()).update("frontend", false);
        EXPECT_EQ(searchAll(workspace, "needle"),
                  (std::vector<std::string>{"frontend:app.js", "frontend:new.js"}));
        expectTidy(workspace);
    }
}

}  // namespace
}  // namespace codesearch
