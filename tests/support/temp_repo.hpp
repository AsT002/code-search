#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/git/git_runner.hpp"
#include "codesearch/util/object_id.hpp"

namespace codesearch::testing {

// A new Git repo in its own temp folder, deleted when the TempRepo goes away.
// Commits use a fixed name and email and ignore the user's hooks and signing
// settings, so tests behave the same on every machine.
class TempRepo {
public:
    explicit TempRepo(const std::string& objectFormat = "sha1");
    ~TempRepo();

    TempRepo(const TempRepo&) = delete;
    TempRepo& operator=(const TempRepo&) = delete;
    TempRepo(TempRepo&&) = delete;
    TempRepo& operator=(TempRepo&&) = delete;

    const std::filesystem::path& dir() const { return dir_; }

    // Writes a file (making folders as needed) and stages it.
    void write(const std::string& path, std::string_view content);
    // Writes a file without staging it, like an uncommitted edit.
    void writeUnstaged(const std::string& path, std::string_view content);
    // Adds a symlink and stages it.
    void symlink(const std::string& path, const std::string& target);
    // Stages a submodule entry pointing at `commit`. No real submodule is set up.
    void addSubmodule(const std::string& path, const ObjectId& commit);
    // Deletes a stored object, as if the repo were damaged. Only works on
    // objects not packed yet, which is all of them in a small new repo.
    void removeObject(const ObjectId& id);
    // The blob ID of a file in HEAD.
    ObjectId blobId(const std::string& path);

    // Commits what is staged and returns the new commit's ID.
    ObjectId commit(const std::string& message = "commit");

    // Runs git in the repo. Throws if it fails.
    GitResult git(const std::vector<std::string>& args);

private:
    std::filesystem::path dir_;
};

}  // namespace codesearch::testing
