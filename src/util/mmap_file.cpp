#include "codesearch/util/mmap_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace codesearch {

namespace {

[[noreturn]] void fail(const std::filesystem::path& path, const char* step, int error) {
    throw std::runtime_error("could not " + std::string(step) + " " + path.string() + ": " +
                             std::strerror(error));  // NOLINT(concurrency-mt-unsafe)
}

}  // namespace

MmapFile::MmapFile(const std::filesystem::path& path) {
    // O_NONBLOCK: opening a FIFO (named pipe) would otherwise wait until
    // something opens the other end, which may never happen. We reject
    // FIFOs below anyway. For a regular file the flag changes nothing.
    const int fd = ::open(path.c_str(),  // NOLINT(cppcoreguidelines-pro-type-vararg)
                          O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fail(path, "open", errno);
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        fail(path, "read", error);
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        throw std::runtime_error(path.string() + " is not a regular file");
    }

    size_ = static_cast<std::size_t>(info.st_size);
    if (size_ > 0) {  // mmap refuses a length of 0
        const void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
            const int error = errno;
            ::close(fd);
            fail(path, "map", error);
        }
        data_ = static_cast<const char*>(mapped);
    }
    ::close(fd);  // the mapping stays valid without the file descriptor
}

MmapFile::~MmapFile() {
    if (data_ != nullptr) {
        // munmap wants a non-const pointer. We never write through it.
        ::munmap(const_cast<char*>(data_), size_);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
}

}  // namespace codesearch
