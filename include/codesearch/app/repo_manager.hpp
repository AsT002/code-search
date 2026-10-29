#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/app/index_commit.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/repo/registry.hpp"
#include "codesearch/repo/workspace.hpp"

namespace codesearch {

// Clones a GitHub repo into a folder. The real one is cloneShallow; tests
// pass their own, so they don't need the network.
using Cloner = std::function<void(const std::string& url, const std::filesystem::path& dest)>;

struct AddResult {
    RepoEntry repo;
    IngestReport report;
};

// Adds and removes repos. Each change holds the workspace lock from start to
// finish, and cleans up leftovers both before and after its own work.
// Methods throw std::runtime_error (or a subclass) with a message for the user.
class RepoManager {
public:
    RepoManager(Workspace workspace, Cloner cloner);

    // Called once if another code-search command holds the lock and we
    // have to wait for it.
    void setOnWait(std::function<void()> onWait) { onWait_ = std::move(onWait); }

    // For tests: called with the name of each step as it finishes ("locked",
    // "cloned", "written", "moved", "saved", "cleaned"), so a test can stop
    // the process there.
    void setOnStep(std::function<void(std::string_view)> onStep) { onStep_ = std::move(onStep); }

    // `source` is a local folder or a GitHub URL. An empty `name` means use
    // the folder's or the GitHub repo's name.
    AddResult add(const std::string& source, const std::string& name);

    // Forgets the repo and deletes its index files, and its clone if we own
    // one. Never touches a local repo's folder. Returns what was removed.
    RepoEntry remove(const std::string& name);

private:
    void step(std::string_view name) const;

    Workspace workspace_;
    Cloner cloner_;
    std::function<void()> onWait_;
    std::function<void(std::string_view)> onStep_;
};

// The registry as it is now. Doesn't take the lock.
Registry loadRegistry(const Workspace& workspace);

struct OpenRepo {
    std::string name;
    std::unique_ptr<ShardReader> index;  // a pointer, because ShardReader can't be moved
};

// Opens the current index of every repo, sorted by name, or only of the repo
// called `onlyName` if it isn't empty. Doesn't take the lock, so it works
// while a change runs. If an index file was deleted between reading the
// registry and opening the file, the registry changed: read it again and
// retry once. Throws std::runtime_error if `onlyName` isn't a repo, and
// IndexFileError (naming the repo) if an index is damaged.
std::vector<OpenRepo> openRepos(const Workspace& workspace, const std::string& onlyName);

}  // namespace codesearch
