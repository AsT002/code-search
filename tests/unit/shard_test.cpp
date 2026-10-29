// Writing index files and reading them back, and how the reader handles
// damaged files.

#include <gtest/gtest.h>
#include <sys/resource.h>

#include <csignal>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"
#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/shard_format.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/index/shard_writer.hpp"
#include "codesearch/search/searcher.hpp"
#include "codesearch/util/endian.hpp"

namespace codesearch {
namespace {

using testing::readFile;
using testing::TempDir;
using testing::writeFile;

ObjectId testCommit() {
    return testing::fakeObjectId('c');
}

MemoryIndex sampleIndex() {
    IndexBuilder builder;
    builder.add("src/main.cpp", "int main() { return run(); }\n");
    builder.add("README.md", "Hello World\nhello again\n");
    builder.add("empty.txt", "");
    builder.add("odd\nname\xFF", std::string("bin\0ary main", 12));
    return builder.build();
}

ShardInfo sampleInfo(PostingCodec codec = PostingCodec::DeltaVarint) {
    return ShardInfo{
        .commit = testCommit(),
        .policyVersion = 7,
        .configFingerprint = 0x1122334455667788ULL,
        .postingCodec = codec,
    };
}

std::vector<DocId> readAll(std::optional<PostingCursor> cursor) {
    std::vector<DocId> docs;
    for (; cursor.has_value() && !cursor->done(); cursor->next()) {
        docs.push_back(cursor->doc());
    }
    return docs;
}

std::string contentOf(const IndexReader& index, DocId doc) {
    std::string out;
    index.withContent(doc, [&](std::string_view content) {
        out = content;
    });
    return out;
}

void expectSameIndex(const MemoryIndex& memory, const ShardReader& file) {
    ASSERT_EQ(file.docCount(), memory.docCount());
    for (DocId doc = 0; doc < memory.docCount(); ++doc) {
        EXPECT_EQ(file.path(doc), memory.path(doc));
        EXPECT_EQ(contentOf(file, doc), contentOf(memory, doc));
    }
    const std::vector<Trigram> trigrams = memory.trigrams();
    EXPECT_EQ(file.header().trigramCount, trigrams.size());
    for (const Trigram trigram : trigrams) {
        EXPECT_EQ(readAll(file.postings(trigram)), readAll(memory.postings(trigram)));
    }
    EXPECT_EQ(file.postings(packTrigram('z', 'z', 'z')), std::nullopt);
    for (const char* text : {"main", "HELLO", "again\n", "zzz", "ary m"}) {
        EXPECT_EQ(search(file, literal(text)), search(memory, literal(text))) << text;
    }
}

TEST(Shard, RoundTripsWithEitherCodec) {
    const TempDir dir;
    const MemoryIndex memory = sampleIndex();
    for (const PostingCodec codec : {PostingCodec::DeltaVarint, PostingCodec::Raw}) {
        writeShard(memory, sampleInfo(codec), dir.file("index.idx"));
        const ShardReader file(dir.file("index.idx"));
        SCOPED_TRACE(static_cast<int>(codec));
        expectSameIndex(memory, file);
        EXPECT_EQ(file.commit(), testCommit());
        EXPECT_EQ(file.header().policyVersion, 7U);
        EXPECT_EQ(file.header().configFingerprint, 0x1122334455667788ULL);
        EXPECT_EQ(file.header().sections[kPostingsSection].codec,
                  static_cast<std::uint32_t>(codec));
        EXPECT_NO_THROW(file.verify());
    }
}

TEST(Shard, EmptyIndex) {
    const TempDir dir;
    const MemoryIndex memory = IndexBuilder().build();
    writeShard(memory, sampleInfo(), dir.file("empty.idx"));
    const ShardReader file(dir.file("empty.idx"));
    EXPECT_EQ(file.docCount(), 0U);
    EXPECT_EQ(file.postings(packTrigram('a', 'b', 'c')), std::nullopt);
    EXPECT_TRUE(search(file, literal("abc")).files.empty());
    EXPECT_THROW(file.path(0), IndexFileError);
    EXPECT_NO_THROW(file.verify());
}

long fileCount(const std::filesystem::path& dir) {
    return std::distance(std::filesystem::directory_iterator(dir),
                         std::filesystem::directory_iterator());
}

TEST(Shard, WriteFailureIsAnError) {
    const TempDir dir;
    EXPECT_THROW(writeShard(sampleIndex(), sampleInfo(), dir.file("missing/folder/x.idx")),
                 std::runtime_error);
    // The target is a folder, so the last step (rename) fails.
    std::filesystem::create_directory(dir.file("folder.idx"));
    EXPECT_THROW(writeShard(sampleIndex(), sampleInfo(), dir.file("folder.idx")),
                 std::runtime_error);
    EXPECT_EQ(fileCount(dir.dir()), 1);  // just the folder; the temp file is gone
}

TEST(Shard, WriteReplacesTheOldFile) {
    const TempDir dir;
    writeFile(dir.file("x.idx"), "old");
    writeShard(sampleIndex(), sampleInfo(), dir.file("x.idx"));
    EXPECT_EQ(ShardReader(dir.file("x.idx")).docCount(), sampleIndex().docCount());
    EXPECT_EQ(fileCount(dir.dir()), 1);
}

// Someone who can change the folder swaps the temp file for a symlink to
// another file while we write. Our writes must not land in that file, and the
// symlink must not end up at the target path.
TEST(Shard, SwappedTempFileCantRedirectTheWrite) {
    const TempDir dir;
    writeFile(dir.file("other.txt"), "someone else's file");
    writeFile(dir.file("x.idx"), "old");
    const auto swapForSymlink = [&](const std::filesystem::path& temp) {
        std::filesystem::remove(temp);
        std::filesystem::create_symlink(dir.file("other.txt"), temp);
    };
    try {
        detail::writeShard(sampleIndex(), sampleInfo(), dir.file("x.idx"), swapForSymlink);
        ADD_FAILURE() << "expected an error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("was replaced"), std::string::npos);
    }
    EXPECT_EQ(readFile(dir.file("other.txt")), "someone else's file");
    EXPECT_FALSE(std::filesystem::is_symlink(dir.file("x.idx")));
    EXPECT_EQ(readFile(dir.file("x.idx")), "old");
    EXPECT_EQ(fileCount(dir.dir()), 2);  // the symlink is cleaned up too
}

// While it exists, writing a file past `bytes` fails, like on a full disk.
// Normally the system stops the process with the SIGXFSZ signal when that
// happens, so this ignores the signal for the same time.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes) : oldHandler_(std::signal(SIGXFSZ, SIG_IGN)) {
        ::getrlimit(RLIMIT_FSIZE, &old_);
        rlimit limit = old_;
        limit.rlim_cur = bytes;
        ::setrlimit(RLIMIT_FSIZE, &limit);
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &old_);
        static_cast<void>(std::signal(SIGXFSZ, oldHandler_));  // nothing to do if it fails
    }

    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    FileSizeLimit(FileSizeLimit&&) = delete;
    FileSizeLimit& operator=(FileSizeLimit&&) = delete;

private:
    void (*oldHandler_)(int) = nullptr;  // a pointer to the old signal handler
    rlimit old_{};
};

TEST(Shard, FailedWriteKeepsTheOldFile) {
    const TempDir dir;
    writeShard(sampleIndex(), sampleInfo(), dir.file("x.idx"));
    const std::string before = readFile(dir.file("x.idx"));
    {
        const FileSizeLimit limit(kHeaderBytes);  // the write fails partway through
        EXPECT_THROW(writeShard(sampleIndex(), sampleInfo(PostingCodec::Raw), dir.file("x.idx")),
                     std::runtime_error);
    }
    EXPECT_EQ(readFile(dir.file("x.idx")), before);
    EXPECT_EQ(fileCount(dir.dir()), 1);  // no temp file left behind
}

// ---- Damaged files ----

void putU32(std::string& bytes, std::size_t at, std::uint32_t value) {
    std::string little;
    appendU32(little, value);
    bytes.replace(at, 4, little);
}

void putU64(std::string& bytes, std::size_t at, std::uint64_t value) {
    std::string little;
    appendU64(little, value);
    bytes.replace(at, 8, little);
}

std::size_t sectionEntry(std::size_t section) {
    return kSectionTableAt + (section * kSectionEntryBytes);
}

// A valid file's bytes, and a place to write damaged copies.
class DamagedShard : public ::testing::Test {
protected:
    DamagedShard() {
        writeShard(sampleIndex(), sampleInfo(), dir_.file("good.idx"));
        good_ = readFile(dir_.file("good.idx"));
        header_ = decodeHeader(good_);
    }

    // Where a section starts in the file.
    std::size_t start(std::size_t section) const {
        return static_cast<std::size_t>(header_.sections.at(section).offset);
    }

    // Writes `bytes` to a file and returns its path.
    std::filesystem::path save(const std::string& bytes) {
        writeFile(dir_.file("bad.idx"), bytes);
        return dir_.file("bad.idx");
    }

    // Expects opening `bytes` to fail with a message containing `message`.
    void expectOpenFails(const std::string& bytes, const std::string& message) {
        try {
            const ShardReader reader(save(bytes));
            ADD_FAILURE() << "opened a damaged file; expected: " << message;
        } catch (const IndexFileError& error) {
            EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
        }
    }

    const std::string& good() const { return good_; }
    const ShardHeader& header() const { return header_; }

private:
    TempDir dir_;
    std::string good_;
    ShardHeader header_;
};

TEST_F(DamagedShard, HeaderChecks) {
    expectOpenFails(good().substr(0, 100), "too small");
    expectOpenFails("", "too small");

    std::string bytes = good();
    bytes.replace(0, 4, "XXXX");
    expectOpenFails(bytes, "wrong magic");

    bytes = good();
    putU32(bytes, kFormatVersionAt, 2);
    expectOpenFails(bytes, "format version 2");

    bytes = good();
    putU64(bytes, kRequiredFeaturesAt, 1);
    expectOpenFails(bytes, "needs features");

    bytes = good();
    bytes[kHashAlgoAt] = 2;
    expectOpenFails(bytes, "commit hash");

    bytes = good();
    putU32(bytes, kSectionCountAt, 4);
    expectOpenFails(bytes, "wrong number of sections");

    bytes = good();
    putU32(bytes, sectionEntry(kPostingsSection) + 16, 7);
    expectOpenFails(bytes, "unknown codec 7");

    bytes = good();
    putU32(bytes, sectionEntry(kPathSection) + 16, 1);  // paths have no codecs
    expectOpenFails(bytes, "unknown codec 1");

    bytes = good();
    putU64(bytes, sectionEntry(kDocSection), start(kDocSection) + 1);
    expectOpenFails(bytes, "not aligned");

    bytes = good();
    putU64(bytes, sectionEntry(kDocSection), start(kPostingsSection));
    expectOpenFails(bytes, "overlaps");

    bytes = good();
    putU64(bytes, sectionEntry(kTrigramSection), 0);  // inside the header
    expectOpenFails(bytes, "overlaps");

    bytes = good();
    putU64(bytes, sectionEntry(kContentSection) + 8, ~0ULL);  // offset + length overflows
    expectOpenFails(bytes, "past the end");

    expectOpenFails(good().substr(0, good().size() - 1), "past the end");  // cut short

    bytes = good();
    putU32(bytes, kTrigramCountAt, header().trigramCount + 1);
    expectOpenFails(bytes, "trigram count");

    bytes = good();
    putU32(bytes, kDocCountAt, header().docCount + 1);
    expectOpenFails(bytes, "file count");
}

TEST_F(DamagedShard, UnknownOptionalFeaturesAreFine) {
    std::string bytes = good();
    putU64(bytes, kOptionalFeaturesAt, 0xFF);
    const ShardReader reader(save(bytes));
    EXPECT_NO_THROW(reader.verify());
}

TEST_F(DamagedShard, ReadChecks) {
    const std::size_t firstTrigram = start(kTrigramSection);
    const Trigram trigram = readU32(good(), firstTrigram);
    const std::size_t firstDoc = start(kDocSection);

    // Posting list outside its section.
    std::string bytes = good();
    putU64(bytes, firstTrigram + 8, 1'000'000);
    {
        const ShardReader reader(save(bytes));
        EXPECT_THROW(reader.postings(trigram), IndexFileError);
        EXPECT_THROW(reader.verify(), IndexFileError);
    }

    // Posting list longer than the number of docs.
    bytes = good();
    putU32(bytes, firstTrigram + 4, header().docCount + 1);
    {
        const ShardReader reader(save(bytes));
        EXPECT_THROW(reader.postings(trigram), IndexFileError);
    }

    // Path outside its section.
    bytes = good();
    putU32(bytes, firstDoc + 8, 1'000'000);
    {
        const ShardReader reader(save(bytes));
        EXPECT_THROW(reader.path(0), IndexFileError);
        EXPECT_EQ(reader.path(1), "README.md");  // other docs still read fine
    }

    // Content outside its section, an unknown codec, and mismatched lengths.
    for (const auto& [at, value] :
         std::vector<std::pair<std::size_t, std::uint32_t>>{{16, 1'000'000}, {28, 1}, {24, 3}}) {
        bytes = good();
        if (at == 28) {
            bytes[firstDoc + at] = static_cast<char>(value);
        } else {
            putU32(bytes, firstDoc + at, value);
        }
        const ShardReader reader(save(bytes));
        EXPECT_THROW(reader.withContent(0, [](std::string_view) {}), IndexFileError) << at;
    }

    const ShardReader reader(save(good()));
    EXPECT_THROW(reader.path(header().docCount), IndexFileError);
    EXPECT_THROW(reader.withContent(header().docCount, [](std::string_view) {}), IndexFileError);
}

TEST_F(DamagedShard, VerifyFindsWhatOpeningDoesNot) {
    // Two trigram entries swapped: the table is no longer sorted.
    const std::size_t table = start(kTrigramSection);
    std::string bytes = good();
    bytes.replace(table, kTrigramEntryBytes,
                  good().substr(table + kTrigramEntryBytes, kTrigramEntryBytes));
    bytes.replace(table + kTrigramEntryBytes, kTrigramEntryBytes,
                  good().substr(table, kTrigramEntryBytes));
    {
        const ShardReader reader(save(bytes));
        EXPECT_THROW(reader.verify(), IndexFileError);
    }

    // One changed content byte: the postings no longer match the text.
    bytes = good();
    const std::size_t main = good().find("int main()");
    ASSERT_NE(main, std::string::npos);
    bytes[main + 4] = 'q';
    {
        const ShardReader reader(save(bytes));
        EXPECT_NO_THROW(reader.path(0));  // normal reads can't tell
        EXPECT_THROW(reader.verify(), IndexFileError);
    }

    // A list that names the wrong file. Only src/main.cpp (doc 0) has "int",
    // so its list is just doc 0. Point it at README.md (doc 1) instead.
    bytes = good();
    std::size_t intList = 0;
    for (std::uint32_t i = 0; i < header().trigramCount; ++i) {
        const std::size_t entry = table + (i * kTrigramEntryBytes);
        if (readU32(good(), entry) == packTrigram('i', 'n', 't')) {
            ASSERT_EQ(readU32(good(), entry + 4), 1U);  // one doc
            intList = start(kPostingsSection) + readU64(good(), entry + 8);
        }
    }
    ASSERT_NE(intList, 0U);
    ASSERT_EQ(bytes[intList], '\x00');
    bytes[intList] = '\x01';
    {
        const ShardReader reader(save(bytes));
        try {
            reader.verify();
            ADD_FAILURE() << "verify passed";
        } catch (const IndexFileError& error) {
            EXPECT_NE(std::string(error.what()).find("doesn't match file 0"), std::string::npos)
                << error.what();
        }
    }

    // A posting list one byte longer than its varints: it now takes the first
    // byte of the next list.
    bytes = good();
    putU32(bytes, table + 16, readU32(good(), table + 16) + 1);
    {
        const ShardReader reader(save(bytes));
        EXPECT_NO_THROW(reader.postings(readU32(good(), table)));
        try {
            reader.verify();
            ADD_FAILURE() << "verify passed";
        } catch (const IndexFileError& error) {
            EXPECT_NE(std::string(error.what()).find("extra bytes"), std::string::npos)
                << error.what();
        }
    }
}

}  // namespace
}  // namespace codesearch
