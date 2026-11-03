#include "codesearch/app/repo_manager.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

#include "codesearch/git/git_repo.hpp"
#include "codesearch/git/git_runner.hpp"
#include "codesearch/index/file_filter.hpp"
#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/shard_format.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/repo/github_url.hpp"
#include "codesearch/repo/publisher.hpp"
#include "codesearch/util/ascii.hpp"
#include "codesearch/util/file_lock.hpp"
#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

// Where a new repo comes from, worked out from what the user typed.
struct Source {
    RepoSource kind = RepoSource::Local;
    std::string location;     // canonical folder path, or the URL in standard form
    std::string defaultName;  // the folder's or GitHub repo's name
};

// True if `path` is `folder` or somewhere inside it. Both must be canonical.
bool isInside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    // mismatch walks both paths part by part and stops at the first difference.
    const auto [stop, ignored] =
        std::mismatch(folder.begin(), folder.end(), path.begin(), path.end());
    return stop == folder.end();
}

Source resolveSource(const std::string& text, const Workspace& workspace) {
    if (text.find("://") != std::string::npos || text.starts_with("git@")) {
        const std::optional<GitHubRepo> github = parseGitHubUrl(text);
        if (!github.has_value()) {
            throw std::runtime_error("only https://github.com/<owner>/<repo> URLs are supported: " +
                                     text);
        }
        return Source{
            .kind = RepoSource::GitHub,
            .location = github->url(),
            .defaultName = github->name,
        };
    }

    std::error_code error;
    const std::filesystem::path path = std::filesystem::canonical(text, error);
    if (error || !std::filesystem::is_directory(path)) {
        throw std::runtime_error("no such folder: " + text);
    }
    if (!isValidUtf8(path.string())) {
        throw std::runtime_error("folder paths must be valid UTF-8: " + text);
    }
    if (isInside(path, std::filesystem::canonical(workspace.root()))) {
        throw std::runtime_error("can't add a folder inside code-search's own data folder " +
                                 workspace.root().string());
    }
    // Git finds the repo from any folder inside it, so a subfolder would
    // index the whole repo, and could add the same repo twice.
    const std::filesystem::path top = repoTopFolder(path);
    if (top != path) {
        throw std::runtime_error(path.string() + " is inside the repo " + top.string() + ". Add " +
                                 top.string() + " instead");
    }
    return Source{
        .kind = RepoSource::Local,
        .location = path.string(),
        .defaultName = path.filename().string(),
    };
}

// The error for a registry.json that's gone while index files or clones
// are still there.
std::runtime_error missingRegistry(const Workspace& workspace) {
    return std::runtime_error(workspace.registryFile().string() +
                              ", which lists the added repos, is missing, but index files or "
                              "clones are still in " +
                              workspace.root().string() +
                              ". Put back a copy of registry.json, or remove that folder to "
                              "start over");
}

// The registry, for a change that holds the lock. Cleanup deletes every
// index file and clone the registry doesn't list, so a registry.json that's
// gone missing would delete them all. If it's missing while there are index
// files or clones, refuse. If it's missing with none there, write an empty
// one first: then a change killed before its first save still leaves a
// registry, and the next cleanup removes what it left.
Registry loadForChange(const Workspace& workspace, RegistryStore& store) {
    if (!std::filesystem::exists(workspace.registryFile())) {
        if (holdsIndexesOrClones(workspace)) {
            throw missingRegistry(workspace);
        }
        store.save(Registry{});
    }
    return store.load();
}

// True if `repo` is the repo `source` names. GitHub owner and repo names
// don't depend on case, so github.com/Owner/Repo is github.com/owner/repo.
bool sameLocation(const RepoEntry& repo, const Source& source) {
    if (repo.source != source.kind) {
        return false;
    }
    if (source.kind == RepoSource::Local) {
        return repo.location == source.location;
    }
    return std::equal(repo.location.begin(), repo.location.end(), source.location.begin(),
                      source.location.end(), [](char a, char b) {
                          return foldAscii(a) == foldAscii(b);
                      });
}

std::string pickName(const std::string& requested, const Source& source, const Registry& registry) {
    const std::string name = requested.empty() ? source.defaultName : requested;
    if (!isValidRepoName(name)) {
        if (requested.empty()) {
            throw std::runtime_error("\"" + name + "\" can't be a repo name. Pick one with --name");
        }
        throw std::runtime_error(
            "repo names can only use letters, digits, '.', '_' and '-', and "
            "can't start with '.' or '-'");
    }
    if (registry.find(name) != nullptr) {
        throw std::runtime_error("a repo named " + name +
                                 " already exists. Pick another name with --name");
    }
    return name;
}

// What the header of an index of `commit` built now records.
ShardInfo shardInfoFor(const ObjectId& commit, const FilterConfig& config) {
    return ShardInfo{
        .commit = commit,
        .policyVersion = kPolicyVersion,
        .configFingerprint = configFingerprint(config),
    };
}

// Why the index `current` must be rebuilt to match `wanted`, or an empty
// string if it's fine as it is.
std::string whyRebuild(const Workspace& workspace, const IndexInfo& current,
                       const ShardInfo& wanted) {
    if (current.commit != wanted.commit) {
        return "new commit";
    }
    if (current.formatVersion != kFormatVersion) {
        return "new index format";
    }
    if (current.policyVersion != wanted.policyVersion) {
        return "new indexing rules";
    }
    if (current.configFingerprint != wanted.configFingerprint) {
        return "changed settings";
    }
    try {
        const ShardReader reader(workspace.indexesDir() / current.file);
        if (!matchesRegistry(reader, current)) {
            return "the index file doesn't match the registry";
        }
    } catch (const std::runtime_error& error) {
        return std::string("the index file can't be used: ") + error.what();
    }
    return "";
}

// `registry` with `entry` in it, in place of the entry with the same ID if
// there is one, sorted by name.
Registry withRepo(Registry registry, const RepoEntry& entry) {
    std::erase_if(registry.repos, [&](const RepoEntry& repo) {
        return repo.id == entry.id;
    });
    registry.repos.push_back(entry);
    std::sort(registry.repos.begin(), registry.repos.end(),
              [](const RepoEntry& a, const RepoEntry& b) {
                  return a.name < b.name;
              });
    return registry;
}

}  // namespace

RepoManager::RepoManager(Workspace workspace, Cloner cloner)
    : workspace_(std::move(workspace)), cloner_(std::move(cloner)) {}

void RepoManager::step(std::string_view name) const {
    if (onStep_) {
        onStep_(name);
    }
}

AddResult RepoManager::add(const std::string& source, const std::string& name) {
    workspace_.create();
    const FileLock lock(workspace_.lockFile(), onWait_);
    step("locked");
    JsonRegistryStore store(workspace_.registryFile());
    Registry registry = loadForChange(workspace_, store);
    cleanUp(workspace_, registry);

    try {
        const Source resolved = resolveSource(source, workspace_);
        for (const RepoEntry& repo : registry.repos) {
            if (sameLocation(repo, resolved)) {
                throw std::runtime_error(resolved.location + " is already added as " + repo.name);
            }
        }
        const std::string id = newRepoId(registry);
        const std::string repoName = pickName(name, resolved, registry);

        std::filesystem::path repoDir = resolved.location;
        if (resolved.kind == RepoSource::GitHub) {
            repoDir = workspace_.cloneDir(id);
            cloner_(resolved.location, repoDir);
            step("cloned");
        }

        // A clone we made is opened as exactly that folder (see FindRepo).
        const FindRepo find =
            resolved.kind == RepoSource::GitHub ? FindRepo::BareRepo : FindRepo::FromFolder;
        const GitRepo repo(repoDir, find);
        const std::optional<ObjectId> commit = repo.resolveHead();
        if (!commit.has_value()) {
            throw std::runtime_error(source + " has no commits yet");
        }
        RepoEntry entry{
            .id = id,
            .name = repoName,
            .source = resolved.kind,
            .location = resolved.location,
            // No index file yet. buildAndPublish fills this in.
            .index = IndexInfo{.file = "", .commit = commit.value()},
        };
        const IngestReport report = buildAndPublish(store, registry, entry, repo, commit.value());
        return AddResult{.repo = entry, .report = report};
    } catch (...) {
        // Delete what we made so far: the clone, files in tmp/ and indexes/.
        // `registry` is what's on disk, so nothing it uses gets deleted.
        try {
            cleanUp(workspace_, registry);
        } catch (...) {  // NOLINT(bugprone-empty-catch) keep the first error; next run cleans up
        }
        throw;
    }
}

UpdateResult RepoManager::update(const std::string& name, bool rebuild) {
    workspace_.create();
    const FileLock lock(workspace_.lockFile(), onWait_);
    step("locked");
    JsonRegistryStore store(workspace_.registryFile());
    Registry registry = loadForChange(workspace_, store);
    cleanUp(workspace_, registry);

    const RepoEntry* found = registry.find(name);
    if (found == nullptr) {
        throw UnknownRepoError("no repo named " + name);
    }
    RepoEntry entry = *found;

    try {
        std::filesystem::path repoDir = entry.location;
        if (entry.source == RepoSource::GitHub) {
            workspace_.checkClone(entry.id);
            repoDir = workspace_.cloneDir(entry.id);
            fetchLatest(repoDir);
            step("fetched");
        } else if (repoTopFolder(repoDir) != repoDir) {
            // Git finds a repo from any folder inside it. If this folder
            // stopped being one (its .git deleted), Git would find a repo
            // around it, whose files would replace this repo's index.
            throw std::runtime_error(entry.location +
                                     " is no longer the top folder of a Git repo. Remove it "
                                     "with: code-search remove " +
                                     entry.name);
        }

        const FindRepo find =
            entry.source == RepoSource::GitHub ? FindRepo::BareRepo : FindRepo::FromFolder;
        const GitRepo repo(repoDir, find);
        const std::optional<ObjectId> commit = repo.resolveHead();
        if (!commit.has_value()) {
            throw std::runtime_error(entry.location + " has no commits");
        }
        const std::string reason =
            rebuild
                ? "asked to rebuild"
                : whyRebuild(workspace_, entry.index, shardInfoFor(commit.value(), filterConfig_));
        if (reason.empty()) {
            return UpdateResult{.repo = entry, .reason = "", .report = {}};
        }
        const IngestReport report = buildAndPublish(store, registry, entry, repo, commit.value());
        return UpdateResult{.repo = entry, .reason = reason, .report = report};
    } catch (...) {
        // Same as in add: the old index stays, and this build's files go.
        try {
            cleanUp(workspace_, registry);
        } catch (...) {  // NOLINT(bugprone-empty-catch) keep the first error; next run cleans up
        }
        throw;
    }
}

IngestReport RepoManager::buildAndPublish(RegistryStore& store, Registry& registry,
                                          RepoEntry& entry, const GitRepo& repo,
                                          const ObjectId& commit) {
    const FileFilter filter(filterConfig_);
    const std::unique_ptr<ThreadPool> pool = startPool();
    const CommitIndex built = indexCommit(repo, commit, filter, pool.get());
    const ShardInfo info = shardInfoFor(commit, filter.config());

    RepoEntry updated = entry;
    updated.index = IndexInfo{
        .file = nextIndexFileName(entry.id, entry.index.file),
        .commit = info.commit,
        .formatVersion = kFormatVersion,
        .policyVersion = info.policyVersion,
        .configFingerprint = info.configFingerprint,
    };
    const std::filesystem::path tmpFile =
        writeToTmp(workspace_, built.index, info, updated.index.file);
    step("written");
    moveIntoIndexes(workspace_, tmpFile);
    step("moved");

    Registry newRegistry = withRepo(registry, updated);
    store.save(newRegistry);
    registry = std::move(newRegistry);  // from here on, this is what's on disk
    entry = std::move(updated);
    step("saved");

    cleanUp(workspace_, registry);  // deletes the old index file
    step("cleaned");
    return built.report;
}

RepoEntry RepoManager::remove(const std::string& name) {
    workspace_.create();
    const FileLock lock(workspace_.lockFile(), onWait_);
    step("locked");
    JsonRegistryStore store(workspace_.registryFile());
    Registry registry = loadForChange(workspace_, store);
    cleanUp(workspace_, registry);

    const RepoEntry* found = registry.find(name);
    if (found == nullptr) {
        throw UnknownRepoError("no repo named " + name);
    }
    const RepoEntry removed = *found;
    std::erase_if(registry.repos, [&](const RepoEntry& repo) {
        return repo.name == name;
    });
    store.save(registry);
    step("saved");
    cleanUp(workspace_, registry);
    step("cleaned");
    return removed;
}

bool matchesRegistry(const ShardReader& reader, const IndexInfo& info) {
    const ShardHeader& header = reader.header();
    return reader.commit() == info.commit && header.formatVersion == info.formatVersion &&
           header.policyVersion == info.policyVersion &&
           header.configFingerprint == info.configFingerprint;
}

std::string rebuildHint(const std::string& name) {
    return ". Rebuild it with: code-search update " + name + " --rebuild";
}

Registry loadRegistry(const Workspace& workspace) {
    // Without this, a lost registry.json would look like "nothing added
    // yet", and the add that suggests would then refuse.
    if (!std::filesystem::exists(workspace.registryFile()) && holdsIndexesOrClones(workspace)) {
        throw missingRegistry(workspace);
    }
    return JsonRegistryStore(workspace.registryFile()).load();
}

namespace {

// Opens `repo`'s current index file and checks it matches the registry.
// Returns nullptr if the file is gone and `mayBeGone` is set: the registry
// changed after we read it. Throws IndexFileError naming the repo if the
// file is damaged or doesn't match.
std::unique_ptr<ShardReader> openIndex(const Workspace& workspace, const RepoEntry& repo,
                                       bool mayBeGone) {
    const std::filesystem::path path = workspace.indexesDir() / repo.index.file;
    std::unique_ptr<ShardReader> reader;
    try {
        reader = std::make_unique<ShardReader>(path);
    } catch (const IndexFileError& error) {
        throw RepoIndexError(repo.name, error.what() + rebuildHint(repo.name));
    } catch (const std::runtime_error& error) {
        if (mayBeGone && !std::filesystem::exists(path)) {
            return nullptr;
        }
        throw std::runtime_error(repo.name + ": " + error.what());
    }
    if (!matchesRegistry(*reader, repo.index)) {
        throw RepoIndexError(repo.name,
                             "the index file doesn't match the registry" + rebuildHint(repo.name));
    }
    return reader;
}

// openIndex, through `cache` if there is one. A file that went away is
// never kept, so it can be opened again when it comes back.
std::shared_ptr<ShardReader> openCached(const Workspace& workspace, const RepoEntry& repo,
                                        bool mayBeGone, IndexCache* cache) {
    if (cache == nullptr) {
        return openIndex(workspace, repo, mayBeGone);
    }
    bool wentAway = false;
    std::shared_ptr<ShardReader> reader;
    try {
        reader = cache->get(workspace.indexesDir() / repo.index.file, [&] {
            std::unique_ptr<ShardReader> opened = openIndex(workspace, repo, mayBeGone);
            if (opened == nullptr) {
                // The cache can't keep "nothing", so stop it with an exception
                // and catch it right below.
                wentAway = true;
                throw std::runtime_error("index file went away");
            }
            return opened;
        });
    } catch (...) {
        if (wentAway) {
            return nullptr;
        }
        throw;
    }
    // A file opened earlier was checked against the registry then. The
    // registry may have been read again since, so check it once more.
    if (!matchesRegistry(*reader, repo.index)) {
        throw RepoIndexError(repo.name,
                             "the index file doesn't match the registry" + rebuildHint(repo.name));
    }
    return reader;
}

// Closes the files in `cache` that `registry` no longer names: they were
// replaced by an update, or their repo was removed.
void forgetReplacedFiles(const Workspace& workspace, const Registry& registry, IndexCache& cache) {
    std::vector<std::filesystem::path> current;
    current.reserve(registry.repos.size());
    for (const RepoEntry& repo : registry.repos) {
        current.push_back(workspace.indexesDir() / repo.index.file);
    }
    cache.forgetOthers(current);
}

// Throws if `query` names a repo (repo:x) that isn't added.
void checkReposExist(const Registry& registry, const QueryNode& query) {
    for (const std::string& name : repoNamesIn(query)) {
        if (registry.find(name) == nullptr) {
            throw UnknownRepoError("no repo named " + name);
        }
    }
}

}  // namespace

std::vector<OpenRepo> openRepos(const Workspace& workspace, const QueryNode& query,
                                std::vector<RepoIndexError>* damaged, IndexCache* cache) {
    // An index file goes away when an update replaces it after we read the
    // registry. Then the registry is read again, a few times, since more
    // updates can finish in between.
    constexpr int kAttempts = 5;
    for (int attempt = 1;; ++attempt) {
        const Registry registry = loadRegistry(workspace);
        checkReposExist(registry, query);
        if (cache != nullptr) {
            forgetReplacedFiles(workspace, registry, *cache);
        }

        std::vector<OpenRepo> opened;
        std::vector<RepoIndexError> damagedHere;  // this attempt's, kept if it's the last
        bool fileWentAway = false;
        for (const RepoEntry& repo : registry.repos) {
            if (resolveRepo(query, repo.name) == constant(false)) {
                continue;  // nothing in this repo can match
            }
            std::shared_ptr<ShardReader> reader;
            try {
                reader = openCached(workspace, repo, attempt < kAttempts, cache);
            } catch (const RepoIndexError& error) {
                if (damaged == nullptr) {
                    throw;
                }
                damagedHere.push_back(error);
                continue;  // leave this repo out
            }
            if (reader == nullptr) {
                fileWentAway = true;
                break;
            }
            opened.push_back(OpenRepo{.name = repo.name, .index = std::move(reader)});
        }
        if (!fileWentAway) {
            if (damaged != nullptr) {
                damaged->insert(damaged->end(), damagedHere.begin(), damagedHere.end());
            }
            return opened;
        }
    }
}

}  // namespace codesearch
