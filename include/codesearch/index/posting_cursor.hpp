#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace codesearch {

using DocId = std::uint32_t;

// Walks a sorted list of doc IDs from the front. It doesn't own the list; it
// only points at it, so the list must outlive the cursor.
class PostingCursor {
public:
    explicit PostingCursor(std::span<const DocId> docs) : docs_(docs) {}

    bool done() const { return pos_ == docs_.size(); }

    // The current doc. Only valid while !done().
    DocId doc() const { return docs_[pos_]; }

    // Length of the whole list. Used to start with the rarest list.
    std::size_t size() const { return docs_.size(); }

    void next() { ++pos_; }

    // Moves to the first doc >= target. Never moves backwards.
    void advance(DocId target) {
        // lower_bound does a binary search for the first value >= target.
        const auto start = docs_.begin() + static_cast<std::ptrdiff_t>(pos_);
        const auto found = std::lower_bound(start, docs_.end(), target);
        pos_ = static_cast<std::size_t>(found - docs_.begin());
    }

private:
    std::span<const DocId> docs_;
    std::size_t pos_ = 0;
};

}  // namespace codesearch
