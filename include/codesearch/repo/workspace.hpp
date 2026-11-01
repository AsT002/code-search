#pragma once

#include <filesystem>
#include <string>

namespace codesearch {

// The folder where code-search keeps its data:
//
//   code-search-workspace   marks the folder as ours
//   registry.json           the repos and which index file is current for each
//   lock                    held during add, update, remove and cleanup
//   repos/                  clones we own (GitHub repos only)
//   indexes/                index files
//   tmp/                    builds in progress
//
// Only paths are worked out here. Nothing is read or written except by create().
class Workspace {
public:
    explicit Workspace(const std::filesystem::path& root);

    // $CODE_SEARCH_HOME if it's set, otherwise ~/.code-search. Throws
    // std::runtime_error if neither can be worked out.
    static std::filesystem::path defaultRoot();

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path markerFile() const { return root_ / "code-search-workspace"; }
    std::filesystem::path registryFile() const { return root_ / "registry.json"; }
    std::filesystem::path lockFile() const { return root_ / "lock"; }
    std::filesystem::path reposDir() const { return root_ / "repos"; }
    std::filesystem::path indexesDir() const { return root_ / "indexes"; }
    std::filesystem::path tmpDir() const { return root_ / "tmp"; }

    // Where the clone of repo `id` lives.
    std::filesystem::path cloneDir(const std::string& id) const {
        return reposDir() / (id + ".git");
    }

    // Creates the folders and the marker file that don't exist yet, then
    // checks the folders (checkFolders). Throws std::runtime_error first if
    // the root already holds other files (checkRoot).
    void create() const;

    // Throws std::runtime_error unless the root is missing, empty, or already
    // a workspace (it has the marker file or registry.json). Cleanup deletes
    // files in the workspace, so code-search must never move into a folder
    // that holds someone else's files, like ~ itself.
    void checkRoot() const;

    // Throws std::runtime_error unless each of repos/, indexes/ and tmp/ that
    // exists is a real folder directly inside the root, not a symlink. Cleanup
    // deletes inside these folders, so a symlinked one could point it at
    // anything on the machine.
    void checkFolders() const;

    // Throws std::runtime_error unless the clone of repo `id` is a real
    // folder directly inside repos/, with no symlinks anywhere inside it.
    // Update writes to the clone, so a symlink could make Git write to
    // another repo on the machine.
    void checkClone(const std::string& id) const;

private:
    std::filesystem::path root_;
};

}  // namespace codesearch
