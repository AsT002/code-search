#include "codesearch/index/posting_cursor.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/util/endian.hpp"
#include "codesearch/util/varint.hpp"

namespace codesearch {
namespace {

std::string rawBytes(const std::vector<DocId>& docs) {
    std::string out;
    for (const DocId doc : docs) {
        appendU32(out, doc);
    }
    return out;
}

std::string varintBytes(const std::vector<DocId>& docs) {
    std::string out;
    DocId previous = 0;
    for (const DocId doc : docs) {
        appendVarint(out, doc - previous);
        previous = doc;
    }
    return out;
}

std::vector<DocId> readAll(PostingCursor cursor) {
    std::vector<DocId> docs;
    for (; !cursor.done(); cursor.next()) {
        docs.push_back(cursor.doc());
    }
    return docs;
}

// Moves a cursor to each target in turn and records where it lands.
std::vector<DocId> landings(PostingCursor cursor, const std::vector<DocId>& targets) {
    std::vector<DocId> out;
    for (const DocId target : targets) {
        cursor.advance(target);
        out.push_back(cursor.done() ? 9999 : cursor.doc());
    }
    return out;
}

TEST(PostingCursor, AllThreeSourcesAgree) {
    std::vector<DocId> docs;
    for (DocId doc = 3; doc < 2000; doc += 7) {
        docs.push_back(doc);
    }
    const std::string raw = rawBytes(docs);
    const std::string varint = varintBytes(docs);
    const auto count = static_cast<std::uint32_t>(docs.size());
    const PostingCursor memory(docs);
    const PostingCursor fromRaw(raw, PostingCodec::Raw, count, 2000);
    const PostingCursor fromVarint(varint, PostingCodec::DeltaVarint, count, 2000);

    EXPECT_EQ(readAll(memory), docs);
    EXPECT_EQ(readAll(fromRaw), docs);
    EXPECT_EQ(readAll(fromVarint), docs);

    // Short and long jumps, targets on and between entries, and past the end.
    const std::vector<DocId> targets = {0, 3, 4, 10, 11, 500, 501, 1500, 1996, 1997, 5000};
    // The plain way: scan forward from where the last target landed.
    std::vector<DocId> expected;
    std::size_t at = 0;
    for (const DocId target : targets) {
        while (at < docs.size() && docs[at] < target) {
            ++at;
        }
        expected.push_back(at < docs.size() ? docs[at] : 9999);
    }
    EXPECT_EQ(landings(memory, targets), expected);
    EXPECT_EQ(landings(fromRaw, targets), expected);
    EXPECT_EQ(landings(fromVarint, targets), expected);
}

TEST(PostingCursor, AdvanceFindsEveryPosition) {
    // From every start, advancing to every target lands on the first doc >= it.
    const std::vector<DocId> docs = {1, 2, 4, 8, 9, 10, 20, 40, 41, 80};
    for (std::size_t start = 0; start < docs.size(); ++start) {
        for (DocId target = 0; target <= 81; ++target) {
            PostingCursor cursor(docs);
            for (std::size_t i = 0; i < start; ++i) {
                cursor.next();
            }
            cursor.advance(target);
            DocId expected = 9999;
            for (std::size_t i = start; i < docs.size(); ++i) {
                if (docs[i] >= target) {
                    expected = docs[i];
                    break;
                }
            }
            EXPECT_EQ(cursor.done() ? 9999 : cursor.doc(), expected)
                << "start " << start << ", target " << target;
        }
    }
}

TEST(PostingCursor, RejectsDamagedVarintLists) {
    // Ends in the middle of a varint.
    EXPECT_THROW(PostingCursor("\x80", PostingCodec::DeltaVarint, 1, 10), IndexFileError);
    // Says 3 docs but holds 2.
    PostingCursor shortList("\x01\x01", PostingCodec::DeltaVarint, 3, 10);
    shortList.next();
    EXPECT_THROW(shortList.next(), IndexFileError);
    // A gap of 0 would repeat a doc.
    const std::string repeatBytes("\x01\x00", 2);  // a named string: the cursor points into it
    PostingCursor repeat(repeatBytes, PostingCodec::DeltaVarint, 2, 10);
    EXPECT_THROW(repeat.next(), IndexFileError);
    // Doc IDs at or above the limit.
    EXPECT_THROW(PostingCursor("\x0A", PostingCodec::DeltaVarint, 1, 10), IndexFileError);
    PostingCursor tooFar("\x05\x05", PostingCodec::DeltaVarint, 2, 10);
    EXPECT_THROW(tooFar.next(), IndexFileError);
    // A huge gap must not wrap around to a small doc ID.
    std::string wrap = "\x05";
    appendVarint(wrap, 0xFFFFFFFEU);
    PostingCursor wrapping(wrap, PostingCodec::DeltaVarint, 2, 10);
    EXPECT_THROW(wrapping.next(), IndexFileError);
}

TEST(PostingCursor, RejectsDamagedRawLists) {
    EXPECT_THROW(PostingCursor(rawBytes({1, 2}), PostingCodec::Raw, 3, 10), IndexFileError);
    EXPECT_THROW(PostingCursor(rawBytes({10}), PostingCodec::Raw, 1, 10), IndexFileError);
    const std::string unsortedBytes = rawBytes({5, 5});
    PostingCursor unsorted(unsortedBytes, PostingCodec::Raw, 2, 10);
    EXPECT_THROW(unsorted.next(), IndexFileError);
}

TEST(PostingCursor, EmptyListIsDone) {
    const std::vector<DocId> none;
    EXPECT_TRUE(PostingCursor(none).done());
    EXPECT_TRUE(PostingCursor("", PostingCodec::DeltaVarint, 0, 10).done());
}

}  // namespace
}  // namespace codesearch
