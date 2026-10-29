#pragma once

#include <filesystem>
#include <string>

namespace codesearch {

// The folder where code-search keeps its data:
//
//   registry.json   the repos and which index file is current for each
//   lock            held during add, update, remove and cleanup
//   repos/          clones we own (GitHub repos only)
//   indexes/        index files
//   tmp/            builds in progress
//
// Only paths are worked out here. Nothing is read or written except by create().
class Workspace {
public:
    explicit Workspace(const std::filesystem::path& root);

    // $CODE_SEARCH_HOME if it's set, otherwise ~/.code-search. Throws
    // std::runtime_error if neither can be worked out.
    static std::filesystem::path defaultRoot();

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path registryFile() const { return root_ / "registry.json"; }
    std::filesystem::path lockFile() const { return root_ / "lock"; }
    std::filesystem::path reposDir() const { return root_ / "repos"; }
    std::filesystem::path indexesDir() const { return root_ / "indexes"; }
    std::filesystem::path tmpDir() const { return root_ / "tmp"; }

    // Where the clone of repo `id` lives.
    std::filesystem::path cloneDir(const std::string& id) const {
        return reposDir() / (id + ".git");
    }

    // Creates the folders that don't exist yet.
    void create() const;

private:
    std::filesystem::path root_;
};

}  // namespace codesearch
