#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <stdexcept>

#include "codesearch/git/git_repo.hpp"
#include "codesearch/index/file_filter.hpp"
#include "codesearch/index/memory_index.hpp"
#include "codesearch/util/thread_pool.hpp"

namespace codesearch {

struct FileCount {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

// What a build indexed and what it skipped, and why.
struct IngestReport {
    FileCount indexed;
    std::map<SkipReason, FileCount> skipped;  // only reasons that happened
};

struct CommitIndex {
    MemoryIndex index;
    IngestReport report;
};

// Thrown by indexCommit when `cancelled` is set.
class IndexingCancelled : public std::runtime_error {
public:
    IndexingCancelled() : std::runtime_error("indexing was cancelled") {}
};

// Reads every file in `commit` and indexes the ones the filter allows.
// With a pool, each file's trigrams are worked out on its threads while Git
// is read; the index comes out the same either way. Throws GitError if Git
// fails or a file's contents are missing from the repo. If `cancelled` is
// given and becomes true, throws IndexingCancelled before the next file.
CommitIndex indexCommit(const GitRepo& repo, const ObjectId& commit, const FileFilter& filter,
                        ThreadPool* pool = nullptr, const std::atomic<bool>* cancelled = nullptr);

}  // namespace codesearch
