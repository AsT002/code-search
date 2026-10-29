// M2.5 storage experiment. Indexes HEAD of one repo, then compares the storage
// choices for the M3 index file:
//
//   postings: raw u32 vs delta-varint (size and intersection speed)
//   contents: raw vs LZ4 per file (size and search speed)
//
// It also prints peak memory and the size of each index file section.
//
// Usage: storage_experiment <repo>
// Results go in docs/BENCHMARKS.md.

#include <lz4.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/app/index_commit.hpp"
#include "codesearch/search/evaluator.hpp"
#include "codesearch/search/searcher.hpp"
#include "codesearch/util/varint.hpp"

namespace {

using namespace codesearch;  // NOLINT(google-build-using-namespace): small program
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 5;  // timings keep the fastest of 5 rounds
constexpr std::size_t kQueryCount = 300;
constexpr std::uint64_t kDocEntryBytes = 64;  // planned doc table entry size
constexpr std::uint64_t kTrigramEntryBytes = 24;
constexpr std::uint64_t kHeaderBytes = 4096;  // a generous guess, tiny either way

double seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

double mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double peakMemoryMiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return mib(static_cast<std::uint64_t>(usage.ru_maxrss));  // bytes on macOS
#else
    return mib(static_cast<std::uint64_t>(usage.ru_maxrss) * 1024);  // KiB on Linux
#endif
}

// ---- Postings in the two formats ----

// Where one trigram's list sits inside a postings blob.
struct ListRef {
    std::size_t offset = 0;
    std::size_t bytes = 0;
    std::uint32_t count = 0;
};

// All posting lists back to back in one blob, the way the file will hold them.
struct PostingsBlob {
    std::string bytes;
    std::vector<ListRef> lists;  // same order as the trigram list
};

void appendRaw(std::string& out, DocId doc) {
    char little[4];
    for (std::size_t i = 0; i < 4; ++i) {
        little[i] = static_cast<char>((doc >> (8U * i)) & 0xFFU);
    }
    out.append(little, 4);
}

DocId readRaw(std::string_view bytes, std::size_t index) {
    DocId doc = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        doc |= static_cast<DocId>(static_cast<unsigned char>(bytes[(index * 4) + i])) << (8U * i);
    }
    return doc;
}

// Raw u32 list. advance() can jump because every entry is 4 bytes. It
// "gallops": checks 1, 2, 4, 8... entries ahead until it passes the target,
// then binary searches that last step. Short jumps stay cheap, unlike a
// binary search over the whole rest of the list.
class RawCursor {
public:
    RawCursor(std::string_view bytes, std::uint32_t count) : bytes_(bytes), count_(count) {}

    bool done() const { return pos_ == count_; }
    DocId doc() const { return readRaw(bytes_, pos_); }
    std::size_t size() const { return count_; }
    void next() { ++pos_; }

    void advance(DocId target) {
        if (done() || doc() >= target) {
            return;
        }
        // Everything up to `low` is below the target.
        std::size_t low = pos_;
        std::size_t step = 1;
        while (low + step < count_ && readRaw(bytes_, low + step) < target) {
            low += step;
            step *= 2;
        }
        low += 1;
        std::size_t high = std::min<std::size_t>(low + step, count_);
        while (low < high) {
            const std::size_t mid = low + ((high - low) / 2);
            if (readRaw(bytes_, mid) < target) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        pos_ = low;
    }

private:
    std::string_view bytes_;
    std::uint32_t count_;
    std::size_t pos_ = 0;
};

// Delta-varint list: the first doc ID, then the gap to each next one. Gaps
// are small, so most take 1 byte. advance() has to step one entry at a time.
class VarintCursor {
public:
    VarintCursor(std::string_view bytes, std::uint32_t count) : bytes_(bytes), count_(count) {
        next();
    }

    bool done() const { return done_; }
    DocId doc() const { return doc_; }
    std::size_t size() const { return count_; }

    void next() {
        const std::optional<std::uint32_t> gap = readVarint(bytes_, byte_);
        if (!gap.has_value()) {
            done_ = true;
            return;
        }
        doc_ = started_ ? doc_ + gap.value() : gap.value();
        started_ = true;
    }

    void advance(DocId target) {
        while (!done_ && doc_ < target) {
            next();
        }
    }

private:
    std::string_view bytes_;
    std::uint32_t count_;
    std::size_t byte_ = 0;
    DocId doc_ = 0;
    bool started_ = false;
    bool done_ = false;
};

void encodeRaw(PostingsBlob& blob, const std::vector<DocId>& docs) {
    const std::size_t start = blob.bytes.size();
    for (const DocId doc : docs) {
        appendRaw(blob.bytes, doc);
    }
    blob.lists.push_back(
        {start, blob.bytes.size() - start, static_cast<std::uint32_t>(docs.size())});
}

void encodeVarint(PostingsBlob& blob, const std::vector<DocId>& docs) {
    const std::size_t start = blob.bytes.size();
    DocId previous = 0;
    for (const DocId doc : docs) {
        appendVarint(blob.bytes, doc - previous);
        previous = doc;
    }
    blob.lists.push_back(
        {start, blob.bytes.size() - start, static_cast<std::uint32_t>(docs.size())});
}

// Rarest-first intersection, the same steps as findCandidates. A template so
// both cursor types share one copy of the code: the compiler makes one
// version for RawCursor and one for VarintCursor.
template <typename Cursor>
std::vector<DocId> intersect(std::vector<Cursor> cursors) {
    std::sort(cursors.begin(), cursors.end(), [](const Cursor& a, const Cursor& b) {
        return a.size() < b.size();
    });
    std::vector<DocId> result;
    Cursor& lead = cursors[0];
    while (!lead.done()) {
        DocId candidate = lead.doc();
        bool inAll = true;
        for (std::size_t i = 1; i < cursors.size(); ++i) {
            cursors[i].advance(candidate);
            if (cursors[i].done()) {
                return result;
            }
            if (cursors[i].doc() != candidate) {
                candidate = cursors[i].doc();
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

// ---- Queries ----

struct Query {
    std::string text;
    std::vector<std::size_t> lists;  // indexes into the sorted trigram list
};

bool isQueryChar(char c) {
    return c > ' ' && c < 127;  // printable ASCII, no spaces
}

// Short strings copied from random spots in the repo, so most queries find
// something and their frequencies look like real searches. Fixed seed, so
// every run uses the same queries.
std::vector<Query> makeQueries(const MemoryIndex& index, const std::vector<Trigram>& trigrams) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<DocId> pickDoc(0, index.docCount() - 1);
    std::uniform_int_distribution<std::size_t> pickLength(4, 12);

    std::vector<Query> queries;
    for (int attempt = 0; queries.size() < kQueryCount && attempt < 100000; ++attempt) {
        std::string text;
        index.withContent(pickDoc(rng), [&](std::string_view content) {
            const std::size_t length = pickLength(rng);
            if (content.size() <= length) {
                return;
            }
            std::uniform_int_distribution<std::size_t> pickStart(0, content.size() - length);
            const std::string_view part = content.substr(pickStart(rng), length);
            if (std::all_of(part.begin(), part.end(), isQueryChar)) {
                text = part;
            }
        });
        if (text.empty()) {
            continue;
        }
        Query query{text, {}};
        for (const Trigram trigram : extractTrigrams(text)) {
            const auto it = std::lower_bound(trigrams.begin(), trigrams.end(), trigram);
            query.lists.push_back(static_cast<std::size_t>(it - trigrams.begin()));
        }
        queries.push_back(std::move(query));
    }
    return queries;
}

template <typename Cursor>
std::vector<std::vector<DocId>> runQueries(const PostingsBlob& blob,
                                           const std::vector<Query>& queries) {
    std::vector<std::vector<DocId>> results;
    results.reserve(queries.size());
    for (const Query& query : queries) {
        std::vector<Cursor> cursors;
        for (const std::size_t list : query.lists) {
            const ListRef& ref = blob.lists[list];
            cursors.emplace_back(std::string_view(blob.bytes).substr(ref.offset, ref.bytes),
                                 ref.count);
        }
        results.push_back(intersect(cursors));
    }
    return results;
}

// Runs fn kRounds times and returns the fastest time in seconds.
template <typename Fn>
double fastest(Fn fn) {
    double best = 1e30;
    for (int round = 0; round < kRounds; ++round) {
        const Clock::time_point start = Clock::now();
        fn();
        best = std::min(best, seconds(Clock::now() - start));
    }
    return best;
}

// ---- Checking candidates ----

// How much of a search to do on each candidate file.
enum class Work {
    FirstMatch,  // stop at the first match, enough for listing files (-l)
    FullSearch,  // what search() does: find every match, with line and column
};

// One round of checking every candidate of every query.
struct VerifyRound {
    double seconds = 0;
    double decompressSeconds = 0;  // part of `seconds`; 0 for raw contents
    std::uint64_t matchedFiles = 0;
    std::uint64_t matches = 0;  // only counted for FullSearch
};

void checkFile(const Evaluator& evaluator, std::string_view content, Work work,
               VerifyRound& round) {
    if (!evaluator.matches(content)) {
        return;
    }
    ++round.matchedFiles;
    if (work == Work::FullSearch) {
        round.matches += locate(content, evaluator.collectSpans(content)).size();
    }
}

// Reads contents from the index when `compressed` is null, otherwise
// decompresses each file's LZ4 block first.
VerifyRound verifyOnce(const MemoryIndex& index, const std::vector<std::string>* compressed,
                       const std::vector<Evaluator>& evaluators,
                       const std::vector<std::vector<DocId>>& candidates, Work work) {
    VerifyRound round;
    std::string buffer;  // reused for every decompressed file
    const Clock::time_point start = Clock::now();
    for (std::size_t q = 0; q < evaluators.size(); ++q) {
        for (const DocId doc : candidates[q]) {
            index.withContent(doc, [&](std::string_view content) {
                if (compressed != nullptr) {
                    // The real file will store the decoded size in the doc
                    // table. Here we take it from the raw copy.
                    const Clock::time_point decompressStart = Clock::now();
                    const std::string& in = (*compressed)[doc];
                    buffer.resize(content.size());
                    const int written =
                        LZ4_decompress_safe(in.data(), buffer.data(), static_cast<int>(in.size()),
                                            static_cast<int>(buffer.size()));
                    if (written != static_cast<int>(buffer.size())) {
                        throw std::runtime_error("LZ4 could not decompress a file");
                    }
                    round.decompressSeconds += seconds(Clock::now() - decompressStart);
                    content = buffer;
                }
                checkFile(evaluators[q], content, work, round);
            });
        }
    }
    round.seconds = seconds(Clock::now() - start);
    return round;
}

// Runs kRounds rounds and returns the fastest. All its numbers come from that
// same round.
VerifyRound fastestVerify(const MemoryIndex& index, const std::vector<std::string>* compressed,
                          const std::vector<Evaluator>& evaluators,
                          const std::vector<std::vector<DocId>>& candidates, Work work) {
    VerifyRound best = verifyOnce(index, compressed, evaluators, candidates, work);
    for (int round = 1; round < kRounds; ++round) {
        const VerifyRound next = verifyOnce(index, compressed, evaluators, candidates, work);
        if (next.seconds < best.seconds) {
            best = next;
        }
    }
    return best;
}

// ---- The experiment ----

int run(const std::string& path) {
    // Build the in-memory index, as the CLI does.
    const Clock::time_point buildStart = Clock::now();
    const GitRepo repo(path);
    const std::optional<ObjectId> head = repo.resolveHead();
    if (!head.has_value()) {
        std::cerr << "no commits\n";
        return 1;
    }
    const CommitIndex built = indexCommit(repo, head.value(), FileFilter());
    const double buildSeconds = seconds(Clock::now() - buildStart);
    const double buildPeakMiB = peakMemoryMiB();
    const MemoryIndex& index = built.index;
    if (index.docCount() == 0) {
        std::cerr << "nothing to measure: no files were indexed\n";
        return 1;
    }

    // Basic counts for the size formula.
    const std::uint64_t docs = index.docCount();
    std::uint64_t contentBytes = 0;
    std::uint64_t pathBytes = 0;
    for (DocId doc = 0; doc < docs; ++doc) {
        pathBytes += index.path(doc).size();
        index.withContent(doc, [&](std::string_view content) {
            contentBytes += content.size();
        });
    }

    // Write every posting list in both formats.
    const std::vector<Trigram> trigrams = index.trigrams();
    PostingsBlob raw;
    PostingsBlob varint;
    std::uint64_t postingEntries = 0;  // P in the formula
    for (const Trigram trigram : trigrams) {
        std::vector<DocId> list;
        for (PostingCursor cursor = index.postings(trigram).value(); !cursor.done();
             cursor.next()) {
            list.push_back(cursor.doc());
        }
        postingEntries += list.size();
        encodeRaw(raw, list);
        encodeVarint(varint, list);
    }

    // Intersection speed. Both formats must find the same docs.
    const std::vector<Query> queries = makeQueries(index, trigrams);
    if (queries.empty()) {
        std::cerr << "nothing to measure: no text to make queries from\n";
        return 1;
    }
    const auto rawResults = runQueries<RawCursor>(raw, queries);
    if (rawResults != runQueries<VarintCursor>(varint, queries)) {
        std::cerr << "raw and varint postings disagree\n";
        return 1;
    }
    std::uint64_t candidates = 0;
    for (const auto& result : rawResults) {
        candidates += result.size();
    }
    const double rawIntersect = fastest([&] {
        runQueries<RawCursor>(raw, queries);
    });
    const double varintIntersect = fastest([&] {
        runQueries<VarintCursor>(varint, queries);
    });

    // Compress every file on its own with LZ4.
    std::vector<std::string> compressed(docs);
    const Clock::time_point compressStart = Clock::now();
    std::uint64_t lz4Bytes = 0;
    for (DocId doc = 0; doc < docs; ++doc) {
        index.withContent(doc, [&](std::string_view content) {
            const int size = static_cast<int>(content.size());
            std::string& out = compressed[doc];
            out.resize(static_cast<std::size_t>(LZ4_compressBound(size)));
            const int written = LZ4_compress_default(content.data(), out.data(), size,
                                                     static_cast<int>(out.size()));
            out.resize(static_cast<std::size_t>(written));
            lz4Bytes += out.size();
        });
    }
    const double compressSeconds = seconds(Clock::now() - compressStart);

    // Check every candidate of every query, with contents raw or through
    // LZ4. Once stopping at the first match, and once doing a full search.
    std::vector<Evaluator> evaluators;
    evaluators.reserve(queries.size());
    for (const Query& query : queries) {
        evaluators.emplace_back(literal(query.text), SearchOptions{});
    }
    const auto verify = [&](const std::vector<std::string>* source, Work work) {
        return fastestVerify(index, source, evaluators, rawResults, work);
    };
    const VerifyRound firstRaw = verify(nullptr, Work::FirstMatch);
    const VerifyRound firstLz4 = verify(&compressed, Work::FirstMatch);
    const VerifyRound fullRaw = verify(nullptr, Work::FullSearch);
    const VerifyRound fullLz4 = verify(&compressed, Work::FullSearch);
    if (firstLz4.matchedFiles != firstRaw.matchedFiles ||
        fullRaw.matchedFiles != firstRaw.matchedFiles || fullLz4.matches != fullRaw.matches) {
        std::cerr << "raw and LZ4 contents disagree\n";
        return 1;
    }

    // Report.
    const std::uint64_t trigramTable = trigrams.size() * kTrigramEntryBytes;
    const std::uint64_t docTable = docs * kDocEntryBytes;
    const auto total = [&](std::uint64_t postings, std::uint64_t contents) {
        return kHeaderBytes + trigramTable + postings + docTable + pathBytes + contents;
    };
    const auto perSource = [&](std::uint64_t bytes) {
        return static_cast<double>(bytes) / static_cast<double>(contentBytes);
    };

    std::printf("repo            %s\n", path.c_str());
    std::printf("files (N)       %llu\n", static_cast<unsigned long long>(docs));
    std::printf("content (C)     %.1f MiB\n", mib(contentBytes));
    std::printf("trigrams (T)    %zu\n", trigrams.size());
    std::printf("postings (P)    %llu\n", static_cast<unsigned long long>(postingEntries));
    std::printf("build           %.2f s, peak memory %.0f MiB\n", buildSeconds, buildPeakMiB);
    std::printf("\nsections (MiB)  trigram table %.1f, doc table %.1f, paths %.1f\n",
                mib(trigramTable), mib(docTable), mib(pathBytes));
    std::printf(
        "postings        raw %.1f, varint %.1f (%.0f%% of raw)\n", mib(raw.bytes.size()),
        mib(varint.bytes.size()),
        100.0 * static_cast<double>(varint.bytes.size()) / static_cast<double>(raw.bytes.size()));
    std::printf("contents        raw %.1f, lz4 %.1f (%.0f%% of raw), compress %.2f s\n",
                mib(contentBytes), mib(lz4Bytes),
                100.0 * static_cast<double>(lz4Bytes) / static_cast<double>(contentBytes),
                compressSeconds);
    std::printf("\nindex total     raw+raw %.1f MiB (%.2fx source)\n",
                mib(total(raw.bytes.size(), contentBytes)),
                perSource(total(raw.bytes.size(), contentBytes)));
    std::printf("                varint+raw %.1f MiB (%.2fx)\n",
                mib(total(varint.bytes.size(), contentBytes)),
                perSource(total(varint.bytes.size(), contentBytes)));
    std::printf("                varint+lz4 %.1f MiB (%.2fx)\n",
                mib(total(varint.bytes.size(), lz4Bytes)),
                perSource(total(varint.bytes.size(), lz4Bytes)));
    std::printf("\n%zu queries, %llu candidates, %llu matched\n", queries.size(),
                static_cast<unsigned long long>(candidates),
                static_cast<unsigned long long>(firstRaw.matchedFiles));
    std::printf("intersect       raw %.2f ms, varint %.2f ms\n", rawIntersect * 1000,
                varintIntersect * 1000);
    const auto printVerify = [](const char* label, const VerifyRound& raw, const VerifyRound& lz4) {
        std::printf("%s raw %.2f ms, lz4 %.2f ms (%.2f ms of it decompressing)\n", label,
                    raw.seconds * 1000, lz4.seconds * 1000, lz4.decompressSeconds * 1000);
    };
    printVerify("first match    ", firstRaw, firstLz4);
    printVerify("full search    ", fullRaw, fullLz4);
    std::printf("                %llu matches found by the full search\n",
                static_cast<unsigned long long>(fullRaw.matches));
    std::printf("peak memory     %.0f MiB at the end (includes the experiment's copies)\n",
                peakMemoryMiB());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: storage_experiment <repo>\n";
        return 2;
    }
    try {
        return run(argv[1]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
