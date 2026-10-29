#include "codesearch/index/shard_writer.hpp"

#include <sys/stat.h>
#include <unistd.h>  // close, write, and mkstemp on macOS

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "codesearch/index/shard_format.hpp"
#include "codesearch/util/endian.hpp"
#include "codesearch/util/varint.hpp"

namespace codesearch {

namespace {

constexpr std::uint64_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

std::uint64_t alignTo8(std::uint64_t offset) {
    return (offset + 7) / 8 * 8;
}

std::uint32_t checkedU32(std::uint64_t value, const char* what) {
    if (value > kMaxU32) {
        throw std::length_error(std::string(what) + " is too big for an index file");
    }
    return static_cast<std::uint32_t>(value);
}

// The trigram table and the postings section, built together.
struct PostingSections {
    std::string table;
    std::string postings;
};

PostingSections encodePostings(const MemoryIndex& index, const std::vector<Trigram>& trigrams,
                               PostingCodec codec) {
    PostingSections out;
    out.table.reserve(trigrams.size() * kTrigramEntryBytes);
    for (const Trigram trigram : trigrams) {
        const std::uint64_t start = out.postings.size();
        std::uint32_t count = 0;
        DocId previous = 0;
        std::optional<PostingCursor> cursor = index.postings(trigram);
        if (!cursor.has_value()) {
            throw std::logic_error("trigrams() listed a trigram with no posting list");
        }
        for (; !cursor->done(); cursor->next()) {
            if (codec == PostingCodec::Raw) {
                appendU32(out.postings, cursor->doc());
            } else {
                appendVarint(out.postings, cursor->doc() - previous);  // the first gap is the ID
            }
            previous = cursor->doc();
            ++count;
        }
        appendU32(out.table, trigram);
        appendU32(out.table, count);
        appendU64(out.table, start);
        appendU32(out.table, checkedU32(out.postings.size() - start, "a posting list"));
        appendU32(out.table, 0);
    }
    return out;
}

// The doc table and the paths section, built together. Contents are written
// straight from the index later, so here we only work out their offsets.
struct DocSections {
    std::string table;
    std::string paths;
    std::uint64_t contentBytes = 0;
};

DocSections encodeDocs(const MemoryIndex& index) {
    DocSections out;
    out.table.reserve(std::size_t{index.docCount()} * kDocEntryBytes);
    for (DocId doc = 0; doc < index.docCount(); ++doc) {
        const std::string_view path = index.path(doc);
        std::uint32_t contentLength = 0;
        index.withContent(doc, [&](std::string_view content) {
            contentLength = checkedU32(content.size(), "a file");
        });

        appendU64(out.table, out.paths.size());
        appendU32(out.table, checkedU32(path.size(), "a path"));
        appendU32(out.table, contentLength);  // stored length
        appendU64(out.table, out.contentBytes);
        appendU32(out.table, contentLength);  // decoded length, the same for raw
        appendU8(out.table, kContentRaw);
        appendU8(out.table, 0);                   // flags
        appendU16(out.table, 0);                  // language
        out.table.append(kHashFieldBytes, '\0');  // blob ID, not filled in yet

        out.paths += path;
        out.contentBytes += contentLength;
    }
    return out;
}

// A new temp file next to a target path, kept open while we write it.
//
// We write only through the descriptor that mkstemp gave us and never open
// the file by name again. If someone swaps the name for something else, say
// a symlink to another file, our writes still go to our own file.
class TempFile {
public:
    explicit TempFile(const std::filesystem::path& target) {
        // mkstemp replaces the XXXXXX with random characters, then creates
        // and opens the file. The name is in the same folder as the target
        // because rename() can only replace a file in one step on one disk.
        std::string name = target.string() + ".tmp-XXXXXX";
        fd_ = ::mkstemp(name.data());
        if (fd_ < 0) {
            throw std::runtime_error("could not create a temp file next to " + target.string());
        }
        path_ = name;
    }

    ~TempFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // Adds `bytes` to the file. They're collected in a buffer and written in
    // big pieces, since each ::write call costs a trip into the system.
    void write(std::string_view bytes) {
        buffer_ += bytes;
        if (buffer_.size() >= kBufferBytes) {
            flush();
        }
    }

    // Writes what's left, checks that path() still names our file, and
    // closes it. Throws std::runtime_error if any of that fails.
    void finish() {
        flush();
        struct stat ours{};
        struct stat named{};
        // lstat looks at the name itself, not where a symlink points.
        if (::fstat(fd_, &ours) != 0 || ::lstat(path_.c_str(), &named) != 0 ||
            ours.st_dev != named.st_dev || ours.st_ino != named.st_ino) {
            throw std::runtime_error(path_.string() + " was replaced while it was being written");
        }
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) {
            fail(errno);
        }
    }

private:
    static constexpr std::size_t kBufferBytes = std::size_t{1} << 20U;  // 1 MiB

    void flush() {
        std::string_view rest = buffer_;
        while (!rest.empty()) {
            const ssize_t written = ::write(fd_, rest.data(), rest.size());
            if (written < 0) {
                if (errno == EINTR) {  // interrupted by a signal before writing; try again
                    continue;
                }
                fail(errno);
            }
            rest.remove_prefix(static_cast<std::size_t>(written));  // may be a partial write
        }
        buffer_.clear();
    }

    [[noreturn]] void fail(int error) const {
        throw std::runtime_error("could not write " + path_.string() + ": " +
                                 std::strerror(error));  // NOLINT(concurrency-mt-unsafe)
    }

    int fd_ = -1;
    std::filesystem::path path_;
    std::string buffer_;
};

// Writes `bytes`, then zeros up to the next 8-byte boundary.
void writeAligned(TempFile& file, std::string_view bytes, std::uint64_t& offset) {
    file.write(bytes);
    offset += bytes.size();
    const std::uint64_t padded = alignTo8(offset);
    file.write(std::string(padded - offset, '\0'));
    offset = padded;
}

// Everything except the contents, ready to write.
struct Prepared {
    std::string header;
    PostingSections postings;
    DocSections docs;
};

Prepared prepare(const MemoryIndex& index, const ShardInfo& info) {
    Prepared out;
    const std::vector<Trigram> trigrams = index.trigrams();
    out.postings = encodePostings(index, trigrams, info.postingCodec);
    out.docs = encodeDocs(index);

    ShardHeader header;
    header.commitHex = info.commit.hex();
    header.policyVersion = info.policyVersion;
    header.configFingerprint = info.configFingerprint;
    header.docCount = index.docCount();
    header.trigramCount = checkedU32(trigrams.size(), "the trigram list");

    // Lay the sections out one after another, each on an 8-byte boundary.
    const std::array<std::uint64_t, kSectionCount> lengths = {
        out.postings.table.size(), out.postings.postings.size(), out.docs.table.size(),
        out.docs.paths.size(),     out.docs.contentBytes,
    };
    std::uint64_t offset = kHeaderBytes;
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        header.sections.at(i).offset = offset;
        header.sections.at(i).length = lengths.at(i);
        offset = alignTo8(offset + lengths.at(i));
    }
    header.sections[kPostingsSection].codec = static_cast<std::uint32_t>(info.postingCodec);
    out.header = encodeHeader(header);
    return out;
}

void writeAll(TempFile& file, const Prepared& prepared, const MemoryIndex& index) {
    std::uint64_t written = 0;
    writeAligned(file, prepared.header, written);
    writeAligned(file, prepared.postings.table, written);
    writeAligned(file, prepared.postings.postings, written);
    writeAligned(file, prepared.docs.table, written);
    writeAligned(file, prepared.docs.paths, written);
    for (DocId doc = 0; doc < index.docCount(); ++doc) {
        index.withContent(doc, [&](std::string_view content) {
            file.write(content);
        });
    }
    file.finish();
}

}  // namespace

namespace detail {

void writeShard(const MemoryIndex& index, const ShardInfo& info, const std::filesystem::path& path,
                const std::function<void(const std::filesystem::path&)>& afterTempCreated) {
    // Build everything we can before touching the disk.
    const Prepared prepared = prepare(index, info);

    // Write a temp file, then rename it over `path`. Until the rename, any
    // old file at `path` is untouched, so a failure never loses it.
    TempFile temp(path);
    try {
        if (afterTempCreated) {
            afterTempCreated(temp.path());
        }
        writeAll(temp, prepared, index);
        std::filesystem::rename(temp.path(), path);
    } catch (...) {                                     // any error: clean up, then let it carry on
        std::error_code ignored;                        // we're already failing; keep that error
        std::filesystem::remove(temp.path(), ignored);  // a symlink is removed, not followed
        throw;
    }
}

}  // namespace detail

void writeShard(const MemoryIndex& index, const ShardInfo& info,
                const std::filesystem::path& path) {
    detail::writeShard(index, info, path, nullptr);
}

}  // namespace codesearch
