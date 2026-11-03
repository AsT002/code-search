#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "codesearch/index/shard_reader.hpp"

namespace codesearch {

// Index files kept open between searches, for the long-running server. The
// CLI opens each file once per search and doesn't need it.
//
// At most `maxOpen` files stay open. When one more is needed, the one used
// longest ago is closed. A search that still uses a file it got from here
// keeps it open until it's done, because each holds a shared_ptr: the file
// closes when the last shared_ptr to it goes away.
//
// Index files are never changed after they're written. An update writes a
// new file with a new name, so a cached file is never out of date; it just
// stops being current, and forgetOthers() closes it.
//
// Safe to use from several threads at once.
class IndexCache {
public:
    // `maxOpen` must be at least 1.
    explicit IndexCache(std::size_t maxOpen);

    // The open file at `path`, or, if it isn't open yet, the one `open`
    // returns, which is then kept. If `open` throws, nothing is kept and the
    // error goes to the caller. `open` runs while the cache is locked.
    std::shared_ptr<ShardReader> get(const std::filesystem::path& path,
                                     const std::function<std::unique_ptr<ShardReader>()>& open);

    // Closes every file except those in `current`. Searches still using one
    // keep it until they finish.
    void forgetOthers(const std::vector<std::filesystem::path>& current);

    // How many files are kept now. For tests.
    std::size_t size() const;

private:
    struct Entry {
        std::filesystem::path path;
        std::shared_ptr<ShardReader> reader;
        std::uint64_t lastUsed = 0;  // a counter, bigger means more recent
    };

    std::size_t maxOpen_;
    mutable std::mutex mutex_;  // `mutable` so size(), a const method, can lock it
    std::vector<Entry> entries_;
    std::uint64_t clock_ = 0;
};

}  // namespace codesearch
