#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/app/index_commit.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/query/ast.hpp"
#include "codesearch/repo/registry.hpp"
#include "codesearch/repo/workspace.hpp"
#include "codesearch/search/repo_search.hpp"

namespace codesearch {

// Clones a GitHub repo into a folder. The real one is cloneShallow; tests
// pass their own, so they don't need the network.
using Cloner = std::function<void(const std::string& url, const std::filesystem::path& dest)>;

struct AddResult {
    RepoEntry repo;
    IngestReport report;
};

struct UpdateResult {
    RepoEntry repo;       // as it is now
    std::string reason;   // why it was rebuilt, like "new commit"; empty if it wasn't
    IngestReport report;  // only filled in if it was rebuilt
};

// Adds, updates and removes repos. Each change holds the workspace lock from start to
// finish, and cleans up leftovers both before and after its own work.
// Methods throw std::runtime_error (or a subclass) with a message for the user.
class RepoManager {
public:
    RepoManager(Workspace workspace, Cloner cloner);

    // Called once if another code-search command holds the lock and we
    // have to wait for it.
    void setOnWait(std::function<void()> onWait) { onWait_ = std::move(onWait); }

    // For tests: called with the name of each step as it finishes ("locked",
    // "cloned" or "fetched", "written", "moved", "saved", "cleaned"), so a
    // test can stop the process there.
    void setOnStep(std::function<void(std::string_view)> onStep) { onStep_ = std::move(onStep); }

    // The filter settings new builds use. Users can't change them yet; tests
    // do, to check that a changed setting causes a rebuild.
    void setFilterConfig(FilterConfig config) { filterConfig_ = std::move(config); }

    // `source` is a local folder or a GitHub URL. An empty `name` means use
    // the folder's or the GitHub repo's name.
    AddResult add(const std::string& source, const std::string& name);

    // Fetches a GitHub repo's latest commit (a local repo is read as it is),
    // then rebuilds its index if anything changed: the commit, the index
    // format, the filter rules or settings, or the index file doesn't open
    // or doesn't match the registry. `rebuild` builds it anyway.
    UpdateResult update(const std::string& name, bool rebuild);

    // Forgets the repo and deletes its index files, and its clone if we own
    // one. Never touches a local repo's folder. Returns what was removed.
    RepoEntry remove(const std::string& name);

private:
    void step(std::string_view name) const;

    // Indexes `commit` of `repo` and publishes it as the next index file of
    // `entry`, then saves `registry` with the new `entry` in it. Updates
    // `entry` and `registry` to match what's on disk.
    IngestReport buildAndPublish(RegistryStore& store, Registry& registry, RepoEntry& entry,
                                 const GitRepo& repo, const ObjectId& commit);

    Workspace workspace_;
    Cloner cloner_;
    FilterConfig filterConfig_;
    std::function<void()> onWait_;
    std::function<void(std::string_view)> onStep_;
};

// Text to add to an error about a damaged index of repo `name`, telling the
// user how to rebuild it.
std::string rebuildHint(const std::string& name);

// True if the header of the open index file says what the registry says
// about it: the same commit, format, filter rules and settings. A file that
// opens fine can still be the wrong one, like another repo's index copied
// over it.
bool matchesRegistry(const ShardReader& reader, const IndexInfo& info);

// The registry as it is now. Doesn't take the lock.
Registry loadRegistry(const Workspace& workspace);

struct OpenRepo {
    std::string name;
    std::unique_ptr<ShardReader> index;  // a pointer, because ShardReader can't be moved
};

// Opens the current index of each repo `query` could match in, sorted by
// name: every repo except those where resolveRepo makes the query false. So
// "repo:a foo" opens only a. Doesn't take the lock, so it works while a
// change runs. If an index file was deleted between reading the registry and
// opening the file, the registry changed: read it again and retry, a few
// times. Throws std::runtime_error if the query names a repo that isn't
// added. If an index is damaged or doesn't match the registry
// (matchesRegistry), the error is a RepoIndexError naming the repo, with how
// to rebuild it. With `damaged`, the repo is left out and the error added
// there; without it, the error is thrown.
std::vector<OpenRepo> openRepos(const Workspace& workspace, const QueryNode& query,
                                std::vector<RepoIndexError>* damaged = nullptr);

}  // namespace codesearch
