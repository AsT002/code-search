#include "codesearch/util/mmap_file.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

#include "../support/temp_dir.hpp"

namespace codesearch {
namespace {

using testing::TempDir;
using testing::writeFile;

TEST(MmapFile, MapsTheWholeFile) {
    const TempDir dir;
    const std::string content("some\0bytes\n", 11);
    writeFile(dir.file("a"), content);
    const MmapFile file(dir.file("a"));
    EXPECT_EQ(file.bytes(), content);
}

TEST(MmapFile, EmptyFileGivesNoBytes) {
    const TempDir dir;
    writeFile(dir.file("empty"), "");
    const MmapFile file(dir.file("empty"));
    EXPECT_TRUE(file.bytes().empty());
}

TEST(MmapFile, MissingFileOrFolderIsAnError) {
    const TempDir dir;
    try {
        const MmapFile file(dir.file("missing"));
        FAIL() << "expected an error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("could not open"), std::string::npos);
    }
    EXPECT_THROW(MmapFile{dir.dir()}, std::runtime_error);  // a folder, not a file
}

// A FIFO (named pipe) with no writer. A plain open() of it waits forever for
// a writer, so this runs the open on another thread with a time limit.
TEST(MmapFile, FifoIsRejectedWithoutWaiting) {
    const TempDir dir;
    const std::filesystem::path fifo = dir.file("pipe");
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

    std::future<void> opening = std::async(std::launch::async, [&] {
        const MmapFile file(fifo);
    });
    if (opening.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        ADD_FAILURE() << "opening a FIFO blocked";
        // Open the writing end so the stuck open() returns and the test ends.
        const int flags = O_WRONLY | O_NONBLOCK;
        const int writer =
            ::open(fifo.c_str(), flags);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        opening.wait();
        ::close(writer);
    }
    EXPECT_THROW(opening.get(), std::runtime_error);
}

}  // namespace
}  // namespace codesearch
