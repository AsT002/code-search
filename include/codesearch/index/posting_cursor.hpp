#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace codesearch {

using DocId = std::uint32_t;

// How a posting list is stored in an index file. The numbers are saved in
// the file, so they must never change.
enum class PostingCodec : std::uint32_t {
    Raw = 0,          // 4 bytes per doc ID, little-endian
    DeltaVarint = 1,  // the first doc ID, then the gap to each next one, as varints
};

// Walks a sorted list of doc IDs from the front. It doesn't own the list; it
// only points at it, so the list must outlive the cursor.
//
// A list from an index file is checked as it's read. A broken varint, a doc
// ID at or above the doc count, or IDs that don't go up throw IndexFileError.
class PostingCursor {
public:
    // A list held in memory.
    explicit PostingCursor(std::span<const DocId> docs);

    // A list inside an index file: `count` doc IDs stored with `codec`.
    PostingCursor(std::string_view bytes, PostingCodec codec, std::uint32_t count, DocId docLimit);

    bool done() const { return pos_ == count_; }

    // The current doc. Only valid while !done().
    DocId doc() const { return current_; }

    // Length of the whole list. Used to start with the rarest list.
    std::size_t size() const { return count_; }

    void next();

    // Moves to the first doc >= target. Never moves backwards.
    void advance(DocId target);

private:
    enum class Source : std::uint8_t { Memory, Raw, Varint };

    // The doc ID at position i, without checks. Memory and Raw only.
    DocId entryAt(std::size_t i) const;
    // Reads the doc at pos_ into current_, checking it if it came from a file.
    void load();

    Source source_;
    std::span<const DocId> memory_;  // Memory
    std::string_view bytes_;         // Raw and Varint
    std::size_t count_ = 0;
    DocId docLimit_ = 0;  // file lists only: every doc ID must be below this
    std::size_t pos_ = 0;
    std::size_t bytePos_ = 0;  // Varint: where the next gap starts
    DocId current_ = 0;
};

}  // namespace codesearch
