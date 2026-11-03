#include "codesearch/repo/workspace.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "codesearch/repo/registry.hpp"

namespace codesearch {

namespace {

constexpr std::string_view kMarkerText =
    "This folder holds code-search's data. code-search deletes files it made\n"
    "here, so don't keep anything else in it.\n";

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
        // A relative path would point somewhere else from each folder
        // code-search runs in, so each would quietly get its own workspace.
        if (!std::filesystem::path(custom).is_absolute()) {
            throw std::runtime_error(
                "CODE_SEARCH_HOME must be a full path, like /home/me/cs, not " + custom);
        }
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
    checkRoot();
    // create_directories does nothing for folders that already exist.
    std::filesystem::create_directories(root_);
    // The marker goes in first, so the folder is never left with our files
    // in it but no marker. It's written in place, not with AtomicFile: it
    // exists as soon as it's opened, so even a write killed halfway leaves
    // it there. Only its name matters, never its text.
    if (!std::filesystem::exists(markerFile())) {
        std::ofstream marker(markerFile());
        marker << kMarkerText;
        marker.close();
        // A stream doesn't throw on its own. If the marker couldn't be made,
        // stop before the folders below, which would leave the folder
        // nonempty but unmarked, and refused from then on.
        if (marker.fail()) {
            throw std::runtime_error("could not create " + markerFile().string());
        }
    }
    std::filesystem::create_directories(reposDir());
    std::filesystem::create_directories(indexesDir());
    std::filesystem::create_directories(tmpDir());
    checkFolders();
}

void Workspace::checkRoot() const {
    if (!std::filesystem::exists(root_)) {
        return;
    }
    if (!std::filesystem::is_directory(root_)) {
        throw std::runtime_error(root_.string() + " isn't a folder");
    }
    if (std::filesystem::exists(markerFile())) {
        return;
    }
    // A registry.json of ours: a workspace made before the marker file
    // existed. Another program's registry.json doesn't count; it would
    // otherwise get our folders made next to it.
    if (std::filesystem::exists(registryFile())) {
        try {
            JsonRegistryStore(registryFile()).load();
            return;
        } catch (const RegistryError&) {  // NOLINT(bugprone-empty-catch) not ours; see below
        }
    }
    // Empty, apart from what an earlier version left if it was killed while
    // writing the marker with AtomicFile: "code-search-workspace.tmp-XXXXXX".
    // Cleanup deletes those. Another code-search may be creating the
    // workspace right now, so a file found is checked against the marker
    // again: create() makes the marker before anything else.
    const std::string leftover = markerFile().filename().string() + ".tmp-";
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root_)) {
        if (!entry.path().filename().string().starts_with(leftover) &&
            !std::filesystem::exists(markerFile())) {
            throw std::runtime_error(
                root_.string() +
                " already has files in it, and isn't a code-search folder. Set "
                "CODE_SEARCH_HOME to a new or empty folder");
        }
    }
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
