#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "codesearch/index/posting_cursor.hpp"
#include "codesearch/index/trigram.hpp"

namespace codesearch {

// Read access to one index. The in-memory index implements it now; the
// on-disk index will implement it in M3, so search code works with both.
class IndexReader {
public:
    virtual ~IndexReader() = default;

    virtual std::uint32_t docCount() const = 0;

    // The docs that contain `trigram`, or std::nullopt if no doc does.
    virtual std::optional<PostingCursor> postings(Trigram trigram) const = 0;

    virtual std::string_view path(DocId doc) const = 0;

    // Calls fn with the doc's content. The view is only valid inside fn.
    virtual void withContent(DocId doc, const std::function<void(std::string_view)>& fn) const = 0;
};

}  // namespace codesearch
