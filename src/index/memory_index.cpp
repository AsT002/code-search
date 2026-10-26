#include "codesearch/index/memory_index.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace codesearch {

std::uint32_t MemoryIndex::docCount() const {
    return static_cast<std::uint32_t>(docs_.size());
}

std::optional<PostingCursor> MemoryIndex::postings(Trigram trigram) const {
    const auto it = postings_.find(trigram);
    if (it == postings_.end()) {
        return std::nullopt;
    }
    return PostingCursor(it->second);
}

std::string_view MemoryIndex::path(DocId doc) const {
    return docs_.at(doc).path;
}

void MemoryIndex::withContent(DocId doc, const std::function<void(std::string_view)>& fn) const {
    fn(docs_.at(doc).content);
}

DocId IndexBuilder::add(std::string path, std::string content) {
    if (index_.docs_.size() >= std::numeric_limits<DocId>::max()) {
        throw std::length_error("too many files for one index");
    }
    const auto id = static_cast<DocId>(index_.docs_.size());

    // extractTrigrams returns each trigram once, so each list gets this ID once.
    for (const Trigram trigram : extractTrigrams(content)) {
        index_.postings_[trigram].push_back(id);
    }
    index_.docs_.push_back(MemoryIndex::Doc{std::move(path), std::move(content)});
    return id;
}

MemoryIndex IndexBuilder::build() {
    MemoryIndex finished = std::move(index_);
    index_ = MemoryIndex{};  // leave the builder empty and reusable
    return finished;
}

}  // namespace codesearch
