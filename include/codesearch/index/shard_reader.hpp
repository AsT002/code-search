#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "codesearch/index/index_reader.hpp"
#include "codesearch/index/shard_format.hpp"
#include "codesearch/util/mmap_file.hpp"
#include "codesearch/util/object_id.hpp"

namespace codesearch {

// An index file, mapped into memory and searched in place. Read-only, so one
// reader can be shared between threads.
//
// The file is treated as untrusted. Opening checks the header and section
// table (decodeHeader). Each read checks the ranges it uses. Anything wrong
// throws IndexFileError. verify() does the slow, complete check.
class ShardReader : public IndexReader {
public:
    // Throws std::runtime_error if the file can't be read, IndexFileError if
    // it isn't a valid index file.
    explicit ShardReader(const std::filesystem::path& path);

    std::uint32_t docCount() const override;
    std::optional<PostingCursor> postings(Trigram trigram) const override;
    std::string_view path(DocId doc) const override;
    void withContent(DocId doc, const std::function<void(std::string_view)>& fn) const override;

    const ObjectId& commit() const { return commit_; }
    const ShardHeader& header() const { return header_; }

    // Reads the whole file and checks everything: trigrams sorted, every
    // posting list complete and in order, every path and content in range,
    // and that the posting lists match the trigrams in the contents.
    // Throws IndexFileError at the first problem.
    void verify() const;

private:
    struct TrigramEntry {
        Trigram trigram = 0;
        std::uint32_t docCount = 0;
        std::uint64_t offset = 0;
        std::uint32_t bytes = 0;
    };

    std::string_view section(std::size_t id) const;
    TrigramEntry trigramEntry(std::uint32_t index) const;
    PostingCursor cursorFor(const TrigramEntry& entry) const;
    void verifyPostingList(const TrigramEntry& entry) const;
    std::string_view contentOf(DocId doc) const;

    // Members are set up in this order, each from the one before.
    MmapFile file_;
    ShardHeader header_;
    ObjectId commit_;
};

}  // namespace codesearch
