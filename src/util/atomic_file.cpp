#include "codesearch/util/atomic_file.hpp"

#include <sys/stat.h>
#include <unistd.h>  // close, fsync, write, and mkstemp on macOS

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace codesearch {

namespace {

constexpr std::size_t kBufferBytes = std::size_t{1} << 20U;  // 1 MiB

}  // namespace

AtomicFile::AtomicFile(const std::filesystem::path& target) : target_(target) {
    // mkstemp replaces the XXXXXX with random characters, then creates and
    // opens the file. The temp file is in the same folder as the target
    // because rename() can only replace a file in one step on one disk.
    std::string name = target.string() + ".tmp-XXXXXX";
    fd_ = ::mkstemp(name.data());
    if (fd_ < 0) {
        throw std::runtime_error("could not create a temp file next to " + target.string());
    }
    tempPath_ = name;
}

AtomicFile::~AtomicFile() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (!committed_) {
        std::error_code ignored;                      // nothing useful to do if this fails
        std::filesystem::remove(tempPath_, ignored);  // a symlink is removed, not followed
    }
}

void AtomicFile::write(std::string_view bytes) {
    buffer_ += bytes;
    if (buffer_.size() >= kBufferBytes) {
        flush();
    }
}

void AtomicFile::commit() {
    flush();
    if (::fsync(fd_) != 0) {
        fail("write", errno);
    }

    // Check that the temp name still points at the file we wrote. lstat looks
    // at the name itself, not where a symlink points.
    struct stat ours{};
    struct stat named{};
    if (::fstat(fd_, &ours) != 0 || ::lstat(tempPath_.c_str(), &named) != 0 ||
        ours.st_dev != named.st_dev || ours.st_ino != named.st_ino) {
        throw std::runtime_error(tempPath_.string() + " was replaced while it was being written");
    }

    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
        fail("write", errno);
    }
    std::filesystem::rename(tempPath_, target_);  // throws std::filesystem::filesystem_error
    committed_ = true;
}

void AtomicFile::flush() {
    std::string_view rest = buffer_;
    while (!rest.empty()) {
        const ssize_t written = ::write(fd_, rest.data(), rest.size());
        if (written < 0) {
            if (errno == EINTR) {  // interrupted by a signal before writing; try again
                continue;
            }
            fail("write", errno);
        }
        rest.remove_prefix(static_cast<std::size_t>(written));  // may be a partial write
    }
    buffer_.clear();
}

void AtomicFile::fail(const std::string& step, int error) const {
    throw std::runtime_error("could not " + step + " " + tempPath_.string() + ": " +
                             std::strerror(error));  // NOLINT(concurrency-mt-unsafe)
}

void writeFileAtomically(const std::filesystem::path& path, std::string_view bytes) {
    AtomicFile file(path);
    file.write(bytes);
    file.commit();
}

}  // namespace codesearch
