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

}  // namespace codesearch
