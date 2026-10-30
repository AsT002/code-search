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
#include "codesearch/repo/github_url.hpp"
#include "codesearch/repo/publisher.hpp"
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
    return Source{
        .kind = RepoSource::Local,
        .location = path.string(),
        .defaultName = path.filename().string(),
    };
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
        return "new filter rules";
    }
    if (current.configFingerprint != wanted.configFingerprint) {
        return "changed settings";
    }
    try {
        const ShardReader reader(workspace.indexesDir() / current.file);
        const ShardHeader& header = reader.header();
        if (reader.commit() != current.commit || header.formatVersion != current.formatVersion ||
            header.policyVersion != current.policyVersion ||
            header.configFingerprint != current.configFingerprint) {
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
    Registry registry = store.load();
    cleanUp(workspace_, registry);

    try {
        const Source resolved = resolveSource(source, workspace_);
        for (const RepoEntry& repo : registry.repos) {
            if (repo.location == resolved.location) {
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

        const GitRepo repo(repoDir);
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
    Registry registry = store.load();
    cleanUp(workspace_, registry);

    const RepoEntry* found = registry.find(name);
    if (found == nullptr) {
        throw std::runtime_error("no repo named " + name);
    }
    RepoEntry entry = *found;

    try {
        std::filesystem::path repoDir = entry.location;
        if (entry.source == RepoSource::GitHub) {
            repoDir = workspace_.cloneDir(entry.id);
            fetchLatest(repoDir);
            step("fetched");
        }

        const GitRepo repo(repoDir);
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
    const CommitIndex built = indexCommit(repo, commit, filter);
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
    Registry registry = store.load();
    cleanUp(workspace_, registry);

    const RepoEntry* found = registry.find(name);
    if (found == nullptr) {
        throw std::runtime_error("no repo named " + name);
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

std::string rebuildHint(const std::string& name) {
    return ". Rebuild it with: code-search update " + name + " --rebuild";
}

Registry loadRegistry(const Workspace& workspace) {
    return JsonRegistryStore(workspace.registryFile()).load();
}

std::vector<OpenRepo> openRepos(const Workspace& workspace, const std::string& onlyName) {
    for (int attempt = 1;; ++attempt) {
        const Registry registry = loadRegistry(workspace);
        if (!onlyName.empty() && registry.find(onlyName) == nullptr) {
            throw std::runtime_error("no repo named " + onlyName);
        }

        std::vector<OpenRepo> opened;
        bool fileWentAway = false;
        for (const RepoEntry& repo : registry.repos) {
            if (!onlyName.empty() && repo.name != onlyName) {
                continue;
            }
            const std::filesystem::path path = workspace.indexesDir() / repo.index.file;
            try {
                opened.push_back(
                    OpenRepo{.name = repo.name, .index = std::make_unique<ShardReader>(path)});
            } catch (const IndexFileError& error) {
                throw IndexFileError(repo.name + ": " + error.what() + rebuildHint(repo.name));
            } catch (const std::runtime_error&) {
                if (attempt == 1 && !std::filesystem::exists(path)) {
                    fileWentAway = true;
                    break;
                }
                throw;
            }
        }
        if (!fileWentAway) {
            return opened;
        }
    }
}

}  // namespace codesearch
