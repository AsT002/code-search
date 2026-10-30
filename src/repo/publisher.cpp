#include "codesearch/repo/publisher.hpp"

#include <set>
#include <stdexcept>
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
    std::filesystem::remove_all(real);  // doesn't follow symlinks inside it either
}

// Deletes every entry in `dir` whose name isn't in `keep`.
void removeAllExcept(const std::filesystem::path& dir, const std::set<std::string>& keep) {
    if (!std::filesystem::exists(dir)) {
        return;
    }
    // Collect first: deleting while walking the folder can confuse the walk.
    std::vector<std::filesystem::directory_entry> doomed;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        if (!keep.contains(entry.path().filename().string())) {
            doomed.push_back(entry);
        }
    }
    for (const std::filesystem::directory_entry& entry : doomed) {
        removeEntry(dir, entry);
    }
}

// Deletes the "registry.json.tmp-XXXXXX" files AtomicFile makes while saving
// the registry. One is left behind only if a save was killed partway.
void removeRegistryTempFiles(const Workspace& workspace) {
    if (!std::filesystem::exists(workspace.root())) {
        return;
    }
    const std::string prefix = workspace.registryFile().filename().string() + ".tmp-";
    std::vector<std::filesystem::directory_entry> doomed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(workspace.root())) {
        if (entry.path().filename().string().starts_with(prefix)) {
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
    removeAllExcept(workspace.tmpDir(), {});
    removeAllExcept(workspace.indexesDir(), indexFiles);
    removeAllExcept(workspace.reposDir(), clones);
    removeRegistryTempFiles(workspace);
}

}  // namespace codesearch
