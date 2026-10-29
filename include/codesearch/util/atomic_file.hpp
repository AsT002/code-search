#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace codesearch {

// Writes a file so that it appears all at once, or not at all.
//
// The bytes go to a temp file next to the target. commit() writes them out
// to disk (fsync) and renames the temp file over the target, which replaces
// any old file in one step. Until then the old file is untouched. If the
// AtomicFile goes away without commit(), the temp file is removed.
//
// We write only through the descriptor mkstemp gave us and never open the
// temp file by name again. If someone swaps the name for something else, say
// a symlink to another file, our writes still go to our own file, and
// commit() notices the swap and fails.
//
// Methods throw std::runtime_error on failure.
class AtomicFile {
public:
    explicit AtomicFile(const std::filesystem::path& target);
    ~AtomicFile();

    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    AtomicFile(AtomicFile&&) = delete;
    AtomicFile& operator=(AtomicFile&&) = delete;

    const std::filesystem::path& tempPath() const { return tempPath_; }

    // Adds `bytes` to the file. They're collected in a buffer and written in
    // big pieces, since each ::write call costs a trip into the system.
    void write(std::string_view bytes);

    // Finishes the file and renames it to the target.
    void commit();

private:
    void flush();
    [[noreturn]] void fail(const std::string& step, int error) const;

    std::filesystem::path target_;
    std::filesystem::path tempPath_;
    int fd_ = -1;
    bool committed_ = false;
    std::string buffer_;
};

// Replaces the file at `path` with `bytes`, all at once.
void writeFileAtomically(const std::filesystem::path& path, std::string_view bytes);

}  // namespace codesearch
