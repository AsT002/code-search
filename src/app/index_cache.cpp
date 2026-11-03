#include "codesearch/app/index_cache.hpp"

#include <algorithm>
#include <stdexcept>

namespace codesearch {

IndexCache::IndexCache(std::size_t maxOpen) : maxOpen_(maxOpen) {
    if (maxOpen == 0) {
        throw std::invalid_argument("an index cache must keep at least one file");
    }
}

std::shared_ptr<ShardReader> IndexCache::get(
    const std::filesystem::path& path, const std::function<std::unique_ptr<ShardReader>()>& open) {
    const std::scoped_lock lock(mutex_);
    ++clock_;
    for (Entry& entry : entries_) {
        if (entry.path == path) {
            entry.lastUsed = clock_;
            return entry.reader;
        }
    }

    // Not open yet. Make room first, so there are never more than maxOpen_.
    std::shared_ptr<ShardReader> reader = open();
    if (entries_.size() >= maxOpen_) {
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < entries_.size(); ++i) {
            if (entries_[i].lastUsed < entries_[oldest].lastUsed) {
                oldest = i;
            }
        }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    entries_.push_back(Entry{.path = path, .reader = reader, .lastUsed = clock_});
    return reader;
}

void IndexCache::forgetOthers(const std::vector<std::filesystem::path>& current) {
    const std::scoped_lock lock(mutex_);
    std::vector<Entry> kept;
    for (Entry& entry : entries_) {
        if (std::find(current.begin(), current.end(), entry.path) != current.end()) {
            kept.push_back(std::move(entry));
        }
    }
    entries_ = std::move(kept);
}

std::size_t IndexCache::size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

}  // namespace codesearch
