#include "codesearch/index/posting_cursor.hpp"

#include <algorithm>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/util/endian.hpp"
#include "codesearch/util/varint.hpp"

namespace codesearch {

PostingCursor::PostingCursor(std::span<const DocId> docs)
    : source_(Source::Memory), memory_(docs), count_(docs.size()) {
    load();
}

PostingCursor::PostingCursor(std::string_view bytes, PostingCodec codec, std::uint32_t count,
                             DocId docLimit)
    : source_(codec == PostingCodec::Raw ? Source::Raw : Source::Varint),
      bytes_(bytes),
      count_(count),
      docLimit_(docLimit) {
    if (source_ == Source::Raw && bytes.size() != std::size_t{count} * 4) {
        throw IndexFileError("index file is damaged: a posting list has the wrong size");
    }
    load();
}

DocId PostingCursor::entryAt(std::size_t i) const {
    if (source_ == Source::Memory) {
        return memory_[i];
    }
    return readU32(bytes_, i * 4);
}

void PostingCursor::load() {
    if (done()) {
        return;
    }
    if (source_ == Source::Memory) {
        current_ = memory_[pos_];
        return;
    }

    DocId value = 0;
    if (source_ == Source::Raw) {
        value = entryAt(pos_);
        if (pos_ > 0 && value <= current_) {
            throw IndexFileError("index file is damaged: a posting list is out of order");
        }
    } else {
        const std::optional<std::uint32_t> gap = readVarint(bytes_, bytePos_);
        if (!gap.has_value()) {
            throw IndexFileError("index file is damaged: a posting list ends early");
        }
        if (pos_ == 0) {
            value = gap.value();
        } else if (gap.value() == 0 || gap.value() >= docLimit_ - current_) {
            // A gap of 0 repeats a doc. A gap that reaches docLimit is out of range
            // (written this way so current_ + gap can't overflow).
            throw IndexFileError("index file is damaged: a posting list has a bad gap");
        } else {
            value = current_ + gap.value();
        }
    }
    if (value >= docLimit_) {
        throw IndexFileError("index file is damaged: a posting list names a missing file");
    }
    current_ = value;
}

void PostingCursor::next() {
    ++pos_;
    load();
}

void PostingCursor::advance(DocId target) {
    if (done() || current_ >= target) {
        return;
    }
    if (source_ == Source::Varint) {
        // Varints have different lengths, so we can't jump. Step through.
        while (!done() && current_ < target) {
            next();
        }
        return;
    }

    // "Gallop": look 1, 2, 4, 8... entries ahead until we pass the target,
    // then binary search inside the last step. Short jumps, the usual case,
    // stay cheap. Everything up to `low` is below the target.
    std::size_t low = pos_;
    std::size_t step = 1;
    while (low + step < count_ && entryAt(low + step) < target) {
        low += step;
        step *= 2;
    }
    std::size_t high = std::min(low + step, count_);
    ++low;
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (entryAt(mid) < target) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    pos_ = low;
    if (source_ == Source::Raw) {
        // load() compares with the previous doc, which we skipped past. Use
        // the one just before the new position, which is below the target.
        current_ = pos_ > 0 ? entryAt(pos_ - 1) : 0;
    }
    load();
}

}  // namespace codesearch
