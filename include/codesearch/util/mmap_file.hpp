#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace codesearch {

// A whole file mapped into memory, read-only. The operating system loads the
// pages when they're first read, so opening a big file is cheap. Throws
// std::runtime_error if the file can't be opened or mapped.
//
// The file must not change while it is mapped; index files never do.
class MmapFile {
public:
    explicit MmapFile(const std::filesystem::path& path);
    ~MmapFile();

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;
    MmapFile(MmapFile&&) = delete;
    MmapFile& operator=(MmapFile&&) = delete;

    // The file's bytes. Valid until the MmapFile is destroyed.
    std::string_view bytes() const { return {data_, size_}; }

private:
    const char* data_ = nullptr;  // nullptr for an empty file
    std::size_t size_ = 0;
};

}  // namespace codesearch
