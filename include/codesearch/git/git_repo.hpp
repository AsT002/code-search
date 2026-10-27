#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "codesearch/git/git_runner.hpp"
#include "codesearch/git/object_id.hpp"

namespace codesearch {

enum class EntryKind {
    File,
    Symlink,
    Submodule,
};

// One file listed in a commit.
struct TreeEntry {
    EntryKind kind = EntryKind::File;
    ObjectId id;             // blob ID, or the commit ID for a submodule
    std::uint64_t size = 0;  // in bytes; 0 for submodules
    std::string path;        // raw bytes from Git, not assumed to be UTF-8
};

// A Git repo on this machine. Only reads from it, never changes it.
// Methods throw GitError if Git fails or prints something unexpected.
class GitRepo {
public:
    // Throws GitError if `dir` isn't in a Git repo, or the repo uses SHA-256.
    explicit GitRepo(std::filesystem::path dir);

    const std::filesystem::path& dir() const { return dir_; }

    // The commit HEAD points to right now, or nothing if the current branch
    // has no commits yet. Throws GitError if HEAD points to something that
    // can't be read as a commit. Look this up once, then pass the ID to
    // everything else.
    std::optional<ObjectId> resolveHead() const;

    // Every file in the commit, in every folder, sorted by path.
    std::vector<TreeEntry> listFiles(const ObjectId& commit) const;

private:
    std::filesystem::path dir_;
};

enum class BlobStatus {
    Found,
    Missing,   // the repo doesn't have this object
    TooLarge,  // bigger than the limit; the content was skipped, not read
};

struct Blob {
    BlobStatus status = BlobStatus::Found;
    std::uint64_t size = 0;  // known for Found and TooLarge
    std::string content;     // only set for Found
};

// Reads file contents by blob ID through one long-running `git cat-file --batch`.
// Throws GitError for replies that make no sense, and ProcessError if Git
// stops or its output ends early.
class BlobStream {
public:
    explicit BlobStream(const GitRepo& repo);

    // Asks for one blob and reads the whole reply before returning, so only
    // one request is ever waiting and the pipes can't block each other.
    Blob read(const ObjectId& id, std::uint64_t maxBytes);

private:
    Process git_;
};

}  // namespace codesearch
