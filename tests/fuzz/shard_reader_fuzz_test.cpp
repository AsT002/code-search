// Opens thousands of randomly damaged copies of a valid index file and calls
// every reader method on each. A damaged file may fail with IndexFileError,
// or happen to still read fine, but must never crash, hang or throw anything
// else. Run with the asan preset to also catch bad memory reads.
//
// Fixed seeds, so a failure always comes back the same way.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"
#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/index/shard_writer.hpp"
#include "codesearch/search/searcher.hpp"

namespace codesearch {
namespace {

using testing::readFile;
using testing::TempDir;
using testing::writeFile;

MemoryIndex fuzzIndex() {
    IndexBuilder builder;
    builder.add("a.txt", "alpha beta gamma\nalpha again\n");
    builder.add("b/c.cpp", "int main() { return alpha(); }\n");
    builder.add("empty", "");
    builder.add("d.md", "beta beta beta delta\n");
    for (int i = 0; i < 20; ++i) {
        builder.add("gen" + std::to_string(i), "line " + std::to_string(i * 37) + " alpha\n");
    }
    return builder.build();
}

// Runs fn. Returns false if it threw IndexFileError, which is expected for
// damaged files. Any other exception goes on to fail the test.
template <typename Fn>
bool tolerate(Fn fn) {
    try {
        fn();
        return true;
    } catch (const IndexFileError&) {
        return false;
    }
}

void flipBit(std::string& bytes, std::size_t at, std::size_t bit) {
    const auto byte = static_cast<unsigned char>(bytes[at]);
    bytes[at] = static_cast<char>(byte ^ (1U << bit));
}

// Every reader method, on every doc and every trigram we know of.
void exercise(const ShardReader& reader, const std::vector<Trigram>& trigrams) {
    std::size_t total = 0;  // use what we read, so none of it is skipped
    for (DocId doc = 0; doc <= reader.docCount() && doc < 64; ++doc) {
        tolerate([&] {
            total += reader.path(doc).size();
        });
        tolerate([&] {
            reader.withContent(doc, [&](std::string_view content) {
                for (const char c : content) {
                    total += static_cast<unsigned char>(c);
                }
            });
        });
    }
    for (const Trigram trigram : trigrams) {
        tolerate([&] {
            std::optional<PostingCursor> cursor = reader.postings(trigram);
            for (DocId target = 0; cursor.has_value() && !cursor->done(); target += 3) {
                total += cursor->doc();
                cursor->advance(target);
                if (!cursor->done()) {
                    cursor->next();
                }
            }
        });
    }
    for (const char* text : {"alpha", "beta", "main()", "zzz", "line 3"}) {
        tolerate([&] {
            total += search(reader, literal(text)).files.size();
        });
    }
    tolerate([&] {
        reader.verify();
    });
    EXPECT_GE(total, 0U);
}

TEST(ShardReaderFuzz, DamagedFilesFailCleanly) {
    const TempDir dir;
    const MemoryIndex index = fuzzIndex();
    writeShard(index, ShardInfo{.commit = testing::fakeObjectId('a')}, dir.file("good.idx"));
    const std::string good = readFile(dir.file("good.idx"));
    std::vector<Trigram> trigrams = index.trigrams();
    trigrams.push_back(0);
    trigrams.push_back(0xFFFFFF);

    std::mt19937 rng(12345);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed):
                              // same files every run
    const auto pick = [&](std::size_t lo, std::size_t hi) {
        return std::uniform_int_distribution<std::size_t>(lo, hi)(rng);
    };

    int opened = 0;
    constexpr int kRounds = 3000;
    for (int round = 0; round < kRounds; ++round) {
        std::string bytes = good;
        switch (pick(0, 4)) {
            case 0:  // flip a few bits anywhere
                for (std::size_t n = pick(1, 4); n > 0; --n) {
                    flipBit(bytes, pick(0, bytes.size() - 1), pick(0, 7));
                }
                break;
            case 1:  // flip a bit in the header
                flipBit(bytes, pick(0, kHeaderBytes - 1), pick(0, 7));
                break;
            case 2: {  // put a random number in a 4-byte field
                const std::size_t at = pick(0, (bytes.size() / 4) - 1) * 4;
                const auto value = static_cast<std::uint32_t>(rng());
                for (std::size_t i = 0; i < 4; ++i) {
                    bytes[at + i] = static_cast<char>((value >> (8 * i)) & 0xFFU);
                }
                break;
            }
            case 3:  // cut the file short
                bytes.resize(pick(0, bytes.size() - 1));
                break;
            default:  // random bytes over part of the tables
                for (std::size_t n = pick(1, 16); n > 0; --n) {
                    bytes[pick(kHeaderBytes, bytes.size() - 1)] = static_cast<char>(rng());
                }
                break;
        }

        writeFile(dir.file("bad.idx"), bytes);
        SCOPED_TRACE("round " + std::to_string(round));
        // Rejected on open is fine. Otherwise test every method.
        tolerate([&] {
            const ShardReader reader(dir.file("bad.idx"));
            ++opened;
            exercise(reader, trigrams);
        });
        if (HasFailure()) {
            return;
        }
    }
    // Most damage lands past the header, so most files should still open and
    // get their reads tested. Otherwise this test checks very little.
    EXPECT_GT(opened, kRounds / 3);
}

}  // namespace
}  // namespace codesearch
