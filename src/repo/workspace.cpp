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
}

}  // namespace codesearch
