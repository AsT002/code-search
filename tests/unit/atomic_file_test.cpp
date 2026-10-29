#include "codesearch/util/atomic_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <iterator>
#include <string>

#include "../support/temp_dir.hpp"

namespace codesearch {
namespace {

using testing::readFile;
using testing::TempDir;
using testing::writeFile;

long fileCount(const std::filesystem::path& dir) {
    return std::distance(std::filesystem::directory_iterator(dir),
                         std::filesystem::directory_iterator());
}

TEST(AtomicFile, WritesAndReplaces) {
    const TempDir dir;
    writeFileAtomically(dir.file("a"), "first");
    EXPECT_EQ(readFile(dir.file("a")), "first");
    writeFileAtomically(dir.file("a"), "second");
    EXPECT_EQ(readFile(dir.file("a")), "second");
    EXPECT_EQ(fileCount(dir.dir()), 1);
}

TEST(AtomicFile, NoCommitKeepsTheOldFile) {
    const TempDir dir;
    writeFile(dir.file("a"), "old");
    {
        AtomicFile file(dir.file("a"));
        file.write("new");
        EXPECT_TRUE(std::filesystem::exists(file.tempPath()));
    }  // gone without commit(), like after an error
    EXPECT_EQ(readFile(dir.file("a")), "old");
    EXPECT_EQ(fileCount(dir.dir()), 1);
}

TEST(AtomicFile, BigWritesArriveWhole) {
    const TempDir dir;
    std::string big;
    for (int i = 0; i < 300000; ++i) {
        big += "line " + std::to_string(i) + '\n';  // a few MB, past the buffer size
    }
    AtomicFile file(dir.file("big"));
    file.write(big.substr(0, 100));
    file.write(big.substr(100));
    file.commit();
    EXPECT_EQ(readFile(dir.file("big")), big);
}

TEST(AtomicFile, MissingFolderIsAnError) {
    const TempDir dir;
    EXPECT_THROW(writeFileAtomically(dir.file("missing/a"), "x"), std::runtime_error);
}

}  // namespace
}  // namespace codesearch
