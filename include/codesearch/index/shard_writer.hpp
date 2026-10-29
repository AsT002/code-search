#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "codesearch/index/memory_index.hpp"
#include "codesearch/index/posting_cursor.hpp"
#include "codesearch/util/object_id.hpp"

namespace codesearch {

// What the header records about how the index was built.
struct ShardInfo {
    ObjectId commit;
    std::uint32_t policyVersion = 0;
    std::uint64_t configFingerprint = 0;
    PostingCodec postingCodec = PostingCodec::DeltaVarint;  // picked in M2.5
};

// Writes `index` to an index file at `path`, replacing any file there. It
// writes a temp file in the same folder first and renames it to `path` at the
// end, so a failure leaves the old file as it was. Throws std::runtime_error
// on failure, after removing the temp file.
void writeShard(const MemoryIndex& index, const ShardInfo& info, const std::filesystem::path& path);

namespace detail {

// For tests: writeShard, but it calls `afterTempCreated` with the temp file's
// path right after creating it, so a test can tamper with the folder.
void writeShard(const MemoryIndex& index, const ShardInfo& info, const std::filesystem::path& path,
                const std::function<void(const std::filesystem::path&)>& afterTempCreated);

}  // namespace detail

}  // namespace codesearch
