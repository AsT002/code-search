#include "codesearch/repo/publisher.hpp"

#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "codesearch/index/shard_reader.hpp"

namespace codesearch {

namespace {

// Deletes `entry`, which must be directly inside `dir`.
void removeEntry(const std::filesystem::path& dir, const std::filesystem::directory_entry& entry) {
    if (entry.is_symlink()) {
        std::filesystem::remove(entry.path());  // removes the link, not its target
        return;
    }
    // canonical() follows every symlink and removes "..", giving the real path.
    const std::filesystem::path real = std::filesystem::canonical(entry.path());
    if (real.parent_path() != std::filesystem::canonical(dir)) {
        throw std::runtime_error("refusing to delete " + real.string() + ": it isn't inside " +
                                 dir.string());
    }
    try {
        std::filesystem::remove_all(real);  // doesn't follow symlinks inside it either
    } catch (const std::filesystem::filesystem_error& error) {
        throw std::runtime_error("could not delete " + real.string() +
                                 ", which code-search no longer uses: " + error.code().message() +
                                 ". Check its permissions, or delete it yourself");
    }
}

// True for "r_1c2d3e4f-g0001.idx", the name of an index file of any repo.
bool isIndexFile(std::string_view name) {
    const std::string_view id = name.substr(0, name.find('-'));
    return isValidRepoId(id) && isValidIndexFileName(name, id);
}

// True for "r_1c2d3e4f.git", the name of a clone.
bool isClone(std::string_view name) {
    return name.ends_with(".git") && isValidRepoId(name.substr(0, name.size() - 4));
}

// True for an index file being built in tmp/, or the "<name>.tmp-XXXXXX"
// file AtomicFile writes it to first.
bool isBuildFile(std::string_view name) {
    const std::size_t tmp = name.find(".tmp-");
    return isIndexFile(name.substr(0, tmp));
}

// Deletes every entry in `dir` that code-search made, unless its name is in
// `keep`. `ours` is a function like isIndexFile that says which names we
// make. Anything else is left alone: it isn't ours to delete.
void removeAllExcept(const std::filesystem::path& dir, const std::set<std::string>& keep,
                     bool (*ours)(std::string_view)) {
    if (!std::filesystem::exists(dir)) {
        return;
    }
    // Collect first: deleting while walking the folder can confuse the walk.
    std::vector<std::filesystem::directory_entry> doomed;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (ours(name) && !keep.contains(name)) {
            doomed.push_back(entry);
        }
    }
    for (const std::filesystem::directory_entry& entry : doomed) {
        removeEntry(dir, entry);
    }
}

// Deletes the "registry.json.tmp-XXXXXX" files AtomicFile makes while saving
// the registry. One is left behind only if a save was killed partway. Also
// deletes "code-search-workspace.tmp-XXXXXX", which an earlier version left
// the same way when it wrote the marker file with AtomicFile.
void removeRootTempFiles(const Workspace& workspace) {
    if (!std::filesystem::exists(workspace.root())) {
        return;
    }
    const std::string registryPrefix = workspace.registryFile().filename().string() + ".tmp-";
    const std::string markerPrefix = workspace.markerFile().filename().string() + ".tmp-";
    std::vector<std::filesystem::directory_entry> doomed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(workspace.root())) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(registryPrefix) || name.starts_with(markerPrefix)) {
            doomed.push_back(entry);
        }
    }
    for (const std::filesystem::directory_entry& entry : doomed) {
        removeEntry(workspace.root(), entry);
    }
}

}  // namespace

std::filesystem::path writeToTmp(const Workspace& workspace, const MemoryIndex& index,
                                 const ShardInfo& info, const std::string& fileName) {
    const std::filesystem::path path = workspace.tmpDir() / fileName;
    writeShard(index, info, path);

    // Opening checks the header and sections. The full verify() would take
    // about as long as the build, so we leave that to `code-search verify`.
    const ShardReader reader(path);
    if (reader.commit() != info.commit || reader.docCount() != index.docCount()) {
        throw std::runtime_error("the new index file " + path.string() +
                                 " doesn't match what was built");
    }
    return path;
}

void moveIntoIndexes(const Workspace& workspace, const std::filesystem::path& tmpFile) {
    std::filesystem::rename(tmpFile, workspace.indexesDir() / tmpFile.filename());
}

bool holdsIndexesOrClones(const Workspace& workspace) {
    const auto holds = [](const std::filesystem::path& folder, bool (*ours)(std::string_view)) {
        if (!std::filesystem::exists(folder)) {
            return false;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(folder)) {
            if (ours(entry.path().filename().string())) {
                return true;
            }
        }
        return false;
    };
    return holds(workspace.indexesDir(), isIndexFile) || holds(workspace.reposDir(), isClone);
}

void cleanUp(const Workspace& workspace, const Registry& registry) {
    workspace.checkFolders();
    std::set<std::string> indexFiles;
    std::set<std::string> clones;
    for (const RepoEntry& repo : registry.repos) {
        indexFiles.insert(repo.index.file);
        if (repo.source == RepoSource::GitHub) {
            clones.insert(workspace.cloneDir(repo.id).filename().string());
        }
    }
    removeAllExcept(workspace.tmpDir(), {}, isBuildFile);
    removeAllExcept(workspace.indexesDir(), indexFiles, isIndexFile);
    removeAllExcept(workspace.reposDir(), clones, isClone);
    removeRootTempFiles(workspace);
}

}  // namespace codesearch
