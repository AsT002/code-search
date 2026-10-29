#include "codesearch/index/shard_reader.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/util/endian.hpp"
#include "codesearch/util/varint.hpp"

namespace codesearch {

namespace {

[[noreturn]] void damaged(const std::string& problem) {
    throw IndexFileError("index file is damaged: " + problem);
}

ObjectId commitFrom(const ShardHeader& header) {
    const std::optional<ObjectId> id = ObjectId::fromHex(header.commitHex);
    if (!id.has_value()) {
        // Not reachable: decodeHeader always makes 40 lowercase hex characters.
        throw std::logic_error("bad commit ID from decodeHeader");
    }
    return id.value();
}

// True if [offset, offset + length) fits inside `size` bytes, without overflow.
bool fits(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    return offset <= size && length <= size - offset;
}

}  // namespace

ShardReader::ShardReader(const std::filesystem::path& path)
    : file_(path), header_(decodeHeader(file_.bytes())), commit_(commitFrom(header_)) {}

std::uint32_t ShardReader::docCount() const {
    return header_.docCount;
}

std::string_view ShardReader::section(std::size_t id) const {
    const Section& s = header_.sections.at(id);
    // decodeHeader checked that the section is inside the file.
    return file_.bytes().substr(s.offset, s.length);
}

ShardReader::TrigramEntry ShardReader::trigramEntry(std::uint32_t index) const {
    const std::string_view table = section(kTrigramSection);
    const std::size_t at = std::size_t{index} * kTrigramEntryBytes;
    return TrigramEntry{
        .trigram = readU32(table, at),
        .docCount = readU32(table, at + 4),
        .offset = readU64(table, at + 8),
        .bytes = readU32(table, at + 16),
    };
}

PostingCursor ShardReader::cursorFor(const TrigramEntry& entry) const {
    const std::string_view postings = section(kPostingsSection);
    if (!fits(entry.offset, entry.bytes, postings.size())) {
        damaged("a posting list goes past the end of its section");
    }
    if (entry.docCount == 0 || entry.docCount > header_.docCount) {
        damaged("a posting list has a bad length");
    }
    const auto codec = static_cast<PostingCodec>(header_.sections[kPostingsSection].codec);
    return {postings.substr(entry.offset, entry.bytes), codec, entry.docCount, header_.docCount};
}

std::optional<PostingCursor> ShardReader::postings(Trigram trigram) const {
    // Binary search the sorted trigram table.
    std::uint32_t low = 0;
    std::uint32_t high = header_.trigramCount;
    while (low < high) {
        const std::uint32_t mid = low + ((high - low) / 2);
        if (trigramEntry(mid).trigram < trigram) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == header_.trigramCount) {
        return std::nullopt;
    }
    const TrigramEntry entry = trigramEntry(low);
    if (entry.trigram != trigram) {
        return std::nullopt;
    }
    return cursorFor(entry);
}

std::string_view ShardReader::path(DocId doc) const {
    if (doc >= header_.docCount) {
        throw IndexFileError("doc ID " + std::to_string(doc) + " is out of range");
    }
    const std::string_view table = section(kDocSection);
    const std::size_t at = std::size_t{doc} * kDocEntryBytes;
    const std::uint64_t offset = readU64(table, at);
    const std::uint32_t length = readU32(table, at + 8);
    const std::string_view paths = section(kPathSection);
    if (!fits(offset, length, paths.size())) {
        damaged("a path goes past the end of its section");
    }
    return paths.substr(offset, length);
}

std::string_view ShardReader::contentOf(DocId doc) const {
    if (doc >= header_.docCount) {
        throw IndexFileError("doc ID " + std::to_string(doc) + " is out of range");
    }
    const std::string_view table = section(kDocSection);
    const std::size_t at = std::size_t{doc} * kDocEntryBytes;
    const std::uint32_t stored = readU32(table, at + 12);
    const std::uint64_t offset = readU64(table, at + 16);
    const std::uint32_t decoded = readU32(table, at + 24);
    const std::uint8_t codec = readU8(table, at + 28);
    if (codec != kContentRaw) {
        damaged("a file has unknown content codec " + std::to_string(codec));
    }
    if (decoded != stored) {
        damaged("a raw file has two different lengths");
    }
    const std::string_view contents = section(kContentSection);
    if (!fits(offset, stored, contents.size())) {
        damaged("a file goes past the end of its section");
    }
    return contents.substr(offset, stored);
}

void ShardReader::withContent(DocId doc, const std::function<void(std::string_view)>& fn) const {
    fn(contentOf(doc));  // raw contents need no copy: fn reads the mapped file
}

void ShardReader::verifyPostingList(const TrigramEntry& entry) const {
    // Reading the list to the end makes the cursor check every doc ID.
    PostingCursor walk = cursorFor(entry);
    while (!walk.done()) {
        walk.next();
    }
    // A varint list must use exactly its bytes. Count the varints.
    if (header_.sections[kPostingsSection].codec ==
        static_cast<std::uint32_t>(PostingCodec::DeltaVarint)) {
        const std::string_view bytes = section(kPostingsSection).substr(entry.offset, entry.bytes);
        std::size_t pos = 0;
        for (std::uint32_t n = 0; n < entry.docCount; ++n) {
            readVarint(bytes, pos);
        }
        if (pos != bytes.size()) {
            damaged("a posting list has extra bytes");
        }
    }
}

void ShardReader::verify() const {
    const std::uint32_t trigramCount = header_.trigramCount;

    // The trigram table and every posting list on its own. Keep a cursor at
    // the start of each list for the next step.
    std::vector<Trigram> trigrams;
    trigrams.reserve(trigramCount);
    std::vector<PostingCursor> cursors;
    cursors.reserve(trigramCount);
    for (std::uint32_t i = 0; i < trigramCount; ++i) {
        const TrigramEntry entry = trigramEntry(i);
        if (entry.trigram > 0xFFFFFFU) {
            damaged("a trigram is more than 3 bytes");
        }
        if (i > 0 && entry.trigram <= trigrams.back()) {
            damaged("the trigram table is not sorted");
        }
        verifyPostingList(entry);
        trigrams.push_back(entry.trigram);
        cursors.push_back(cursorFor(entry));
    }

    // Every doc: path and content in range, and the posting lists say exactly
    // which trigrams its content has. Each list's cursor moves forward as we
    // meet its docs in order, so at the end every cursor must be done.
    for (DocId doc = 0; doc < header_.docCount; ++doc) {
        path(doc);
        for (const Trigram trigram : extractTrigrams(contentOf(doc))) {
            const auto it = std::lower_bound(trigrams.begin(), trigrams.end(), trigram);
            if (it == trigrams.end() || *it != trigram) {
                damaged("file " + std::to_string(doc) + " has a trigram missing from the index");
            }
            PostingCursor& cursor = cursors.at(static_cast<std::size_t>(it - trigrams.begin()));
            if (cursor.done() || cursor.doc() != doc) {
                damaged("a posting list doesn't match file " + std::to_string(doc));
            }
            cursor.next();
        }
    }
    for (const PostingCursor& cursor : cursors) {
        if (!cursor.done()) {
            damaged("a posting list names a file without that trigram");
        }
    }
}

}  // namespace codesearch
