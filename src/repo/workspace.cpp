#include "codesearch/repo/workspace.hpp"

#include <cstdlib>
#include <stdexcept>

namespace codesearch {

namespace {

// The value of an environment variable, or "" if it isn't set.
std::string envValue(const char* name) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value == nullptr ? "" : value;
}

}  // namespace

Workspace::Workspace(const std::filesystem::path& root) : root_(std::filesystem::absolute(root)) {}

std::filesystem::path Workspace::defaultRoot() {
    std::string custom = envValue("CODE_SEARCH_HOME");
    if (!custom.empty()) {
        return custom;
    }
    const std::string home = envValue("HOME");
    if (home.empty()) {
        throw std::runtime_error(
            "HOME isn't set, so there's nowhere to keep data. Set "
            "CODE_SEARCH_HOME to a folder");
    }
    return std::filesystem::path(home) / ".code-search";
}

void Workspace::create() const {
    // create_directories does nothing for folders that already exist.
    std::filesystem::create_directories(reposDir());
    std::filesystem::create_directories(indexesDir());
    std::filesystem::create_directories(tmpDir());
    checkFolders();
}

void Workspace::checkFolders() const {
    for (const std::filesystem::path& dir : {reposDir(), indexesDir(), tmpDir()}) {
        // symlink_status looks at the name itself; status() would follow a symlink.
        const std::filesystem::file_status status = std::filesystem::symlink_status(dir);
        if (!std::filesystem::exists(status)) {
            continue;
        }
        // canonical() follows every symlink, so this also catches a root
        // whose subfolder resolves somewhere else.
        if (!std::filesystem::is_directory(status) ||
            std::filesystem::canonical(dir).parent_path() != std::filesystem::canonical(root_)) {
            throw std::runtime_error(dir.string() + " must be a plain folder inside " +
                                     root_.string() +
                                     ", not a symlink or file. Move it away and try again");
        }
    }
}

void Workspace::checkClone(const std::string& id) const {
    const std::filesystem::path clone = cloneDir(id);
    const std::string problem = clone.string() + " must be a plain folder inside " +
                                reposDir().string() + ", with no symlinks in it. ";
    const std::string fix = "Remove the repo and add it again";
    const std::filesystem::file_status status = std::filesystem::symlink_status(clone);
    if (!std::filesystem::exists(status)) {
        throw std::runtime_error(clone.string() + " is missing. " + fix);
    }
    // repos/ itself is checked by checkFolders, and IDs can't hold '/' or
    // "..", so a folder that isn't a symlink is directly inside repos/.
    if (!std::filesystem::is_directory(status)) {
        throw std::runtime_error(problem + fix);
    }
    // recursive_directory_iterator walks every folder inside. It doesn't
    // follow symlinks, so it lists each one as a symlink.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(clone)) {
        if (entry.is_symlink()) {
            std::string message = problem;
            message += entry.path().string() + " is a symlink. " + fix;
            throw std::runtime_error(message);
        }
    }
}

}  // namespace codesearch
