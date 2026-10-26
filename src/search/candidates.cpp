#include "codesearch/search/candidates.hpp"

#include <algorithm>

namespace codesearch {

namespace {

std::vector<DocId> allDocs(const IndexReader& index) {
    std::vector<DocId> docs;
    docs.reserve(index.docCount());  // allocate once instead of growing step by step
    for (DocId doc = 0; doc < index.docCount(); ++doc) {
        docs.push_back(doc);
    }
    return docs;
}

std::vector<DocId> toVector(PostingCursor cursor) {
    std::vector<DocId> docs;
    while (!cursor.done()) {
        docs.push_back(cursor.doc());
        cursor.next();
    }
    return docs;
}

// Docs that are in every list. Starts from the shortest list and stops as soon
// as any list runs out. `cursors` must not be empty.
std::vector<DocId> intersect(std::vector<PostingCursor> cursors) {
    std::sort(cursors.begin(), cursors.end(), [](const PostingCursor& a, const PostingCursor& b) {
        return a.size() < b.size();
    });

    std::vector<DocId> result;
    PostingCursor& lead = cursors[0];
    while (!lead.done()) {
        DocId candidate = lead.doc();
        bool inAll = true;
        for (std::size_t i = 1; i < cursors.size(); ++i) {
            PostingCursor& other = cursors[i];
            other.advance(candidate);
            if (other.done()) {
                return result;  // this list ran out, so no more matches anywhere
            }
            if (other.doc() != candidate) {
                candidate = other.doc();  // no doc before this one can be in every list
                inAll = false;
                break;
            }
        }
        if (inAll) {
            result.push_back(candidate);
            lead.next();
        } else {
            lead.advance(candidate);
        }
    }
    return result;
}

std::vector<DocId> evaluateAnd(const IndexReader& index, const AndQuery& query) {
    std::vector<PostingCursor> cursors;
    // Results of nested queries. Cursors point into these, so they must stay
    // alive until the intersection is done.
    std::vector<std::vector<DocId>> nested;

    for (const TrigramQuery& child : query.children) {
        if (std::holds_alternative<AllQuery>(child.value)) {
            continue;
        }
        if (std::holds_alternative<NoneQuery>(child.value)) {
            return {};
        }
        if (const auto* term = std::get_if<TrigramTerm>(&child.value)) {
            const std::optional<PostingCursor> cursor = index.postings(term->trigram);
            if (!cursor.has_value()) {
                return {};  // no doc has this trigram
            }
            cursors.push_back(cursor.value());
            continue;
        }
        nested.push_back(findCandidates(index, child));
        if (nested.back().empty()) {
            return {};
        }
    }

    for (const std::vector<DocId>& docs : nested) {
        cursors.push_back(PostingCursor(docs));
    }
    if (cursors.empty()) {
        return allDocs(index);  // every child was All
    }
    return intersect(cursors);
}

}  // namespace

std::vector<DocId> findCandidates(const IndexReader& index, const TrigramQuery& query) {
    if (std::holds_alternative<AllQuery>(query.value)) {
        return allDocs(index);
    }
    if (std::holds_alternative<NoneQuery>(query.value)) {
        return {};
    }
    if (const auto* term = std::get_if<TrigramTerm>(&query.value)) {
        const std::optional<PostingCursor> cursor = index.postings(term->trigram);
        if (!cursor.has_value()) {
            return {};
        }
        return toVector(cursor.value());
    }
    return evaluateAnd(index, std::get<AndQuery>(query.value));
}

}  // namespace codesearch
