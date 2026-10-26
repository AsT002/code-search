#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "codesearch/index/index_reader.hpp"

namespace codesearch {

// An index held fully in memory. Made by IndexBuilder.
class MemoryIndex : public IndexReader {
public:
    std::uint32_t docCount() const override;
    std::optional<PostingCursor> postings(Trigram trigram) const override;
    std::string_view path(DocId doc) const override;
    void withContent(DocId doc, const std::function<void(std::string_view)>& fn) const override;

private:
    // IndexBuilder fills in the private fields below.
    friend class IndexBuilder;

    struct Doc {
        std::string path;
        std::string content;
    };

    std::vector<Doc> docs_;                                     // indexed by doc ID
    std::unordered_map<Trigram, std::vector<DocId>> postings_;  // trigram -> sorted doc IDs
};

// Adds files one at a time, then hands over a MemoryIndex.
class IndexBuilder {
public:
    // Returns the new file's doc ID. IDs start at 0 and go up by one, so every
    // posting list comes out sorted.
    DocId add(std::string path, std::string content);

    // Hands over the finished index. The builder is empty afterwards.
    MemoryIndex build();

private:
    MemoryIndex index_;
};

}  // namespace codesearch
