#include "codesearch/util/file_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>  // flock
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace codesearch {

namespace {

[[noreturn]] void fail(const std::filesystem::path& path, int error) {
    throw std::runtime_error("could not lock " + path.string() + ": " +
                             std::strerror(error));  // NOLINT(concurrency-mt-unsafe)
}

// flock, tried again if a signal interrupts it.
int lockRetrying(int fd, int operation) {
    while (true) {
        const int result = ::flock(fd, operation);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

}  // namespace

FileLock::FileLock(const std::filesystem::path& path, const std::function<void()>& onWait) {
    const int flags = O_RDWR | O_CREAT | O_CLOEXEC;
    fd_ = ::open(path.c_str(), flags, 0600);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd_ < 0) {
        fail(path, errno);
    }
    // First try without waiting (LOCK_NB), so we can say we're waiting.
    if (lockRetrying(fd_, LOCK_EX | LOCK_NB) == 0) {
        return;
    }
    const int error = errno;
    if (error != EWOULDBLOCK) {
        ::close(fd_);
        fail(path, error);
    }
    if (onWait) {
        onWait();
    }
    if (lockRetrying(fd_, LOCK_EX) != 0) {
        const int waitError = errno;
        ::close(fd_);
        fail(path, waitError);
    }
}

FileLock::~FileLock() {
    ::close(fd_);  // closing the file releases the lock
}

}  // namespace codesearch
