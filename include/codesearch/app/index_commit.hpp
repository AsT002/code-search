#pragma once

#include <cstdint>
#include <map>

#include "codesearch/git/git_repo.hpp"
#include "codesearch/index/file_filter.hpp"
#include "codesearch/index/memory_index.hpp"

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

// Reads every file in `commit` and indexes the ones the filter allows.
// Throws GitError if Git fails or a file's contents are missing from the repo.
CommitIndex indexCommit(const GitRepo& repo, const ObjectId& commit, const FileFilter& filter);

}  // namespace codesearch
