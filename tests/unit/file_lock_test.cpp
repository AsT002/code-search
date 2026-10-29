#include "codesearch/util/file_lock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>

#include "../support/temp_dir.hpp"

namespace codesearch {
namespace {

using testing::TempDir;

TEST(FileLock, SecondLockWaitsForTheFirst) {
    const TempDir dir;
    std::optional<FileLock> first;
    first.emplace(dir.file("lock"));  // emplace builds the FileLock inside the optional

    std::atomic<bool> waited = false;  // atomic: safe to set on one thread and read on another
    std::future<void> second = std::async(std::launch::async, [&] {
        const FileLock lock(dir.file("lock"), [&] {
            waited = true;
        });
    });
    EXPECT_EQ(second.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);

    first.reset();  // destroys the first FileLock, which releases the lock
    EXPECT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    second.get();
    EXPECT_TRUE(waited);
}

TEST(FileLock, NoWaitWhenFree) {
    const TempDir dir;
    bool waited = false;
    {
        const FileLock lock(dir.file("lock"), [&] {
            waited = true;
        });
    }
    const FileLock again(dir.file("lock"), [&] {
        waited = true;
    });
    EXPECT_FALSE(waited);
}

TEST(FileLock, MissingFolderIsAnError) {
    const TempDir dir;
    EXPECT_THROW(FileLock(dir.file("missing/lock")), std::runtime_error);
}

}  // namespace
}  // namespace codesearch
