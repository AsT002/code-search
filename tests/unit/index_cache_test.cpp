// IndexCache: keeps index files open between searches, up to a limit.

#include "codesearch/app/index_cache.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"
#include "codesearch/index/memory_index.hpp"
#include "codesearch/index/shard_writer.hpp"

namespace codesearch {
namespace {

using testing::TempDir;

// Writes a small index file named `name` and returns its path.
std::filesystem::path writeIndex(const TempDir& dir, const std::string& name) {
    IndexBuilder builder;
    builder.add("a.txt", "hello " + name + "\n");
    const std::filesystem::path path = dir.file(name);
    writeShard(builder.build(), ShardInfo{.commit = testing::fakeObjectId('c')}, path);
    return path;
}

// Opens `path`, and counts how many times it did.
std::function<std::unique_ptr<ShardReader>()> opener(const std::filesystem::path& path,
                                                     int& opens) {
    return [&path, &opens] {
        ++opens;
        return std::make_unique<ShardReader>(path);
    };
}

TEST(IndexCache, OpensEachFileOnce) {
    const TempDir dir;
    const std::filesystem::path a = writeIndex(dir, "a.idx");
    IndexCache cache(4);
    int opens = 0;
    const std::shared_ptr<ShardReader> first = cache.get(a, opener(a, opens));
    const std::shared_ptr<ShardReader> second = cache.get(a, opener(a, opens));
    EXPECT_EQ(opens, 1);
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(cache.size(), 1U);
}

TEST(IndexCache, ClosesTheFileUsedLongestAgo) {
    const TempDir dir;
    const std::filesystem::path a = writeIndex(dir, "a.idx");
    const std::filesystem::path b = writeIndex(dir, "b.idx");
    const std::filesystem::path c = writeIndex(dir, "c.idx");
    IndexCache cache(2);
    int opens = 0;
    cache.get(a, opener(a, opens));
    cache.get(b, opener(b, opens));
    cache.get(a, opener(a, opens));  // now b is the one used longest ago
    cache.get(c, opener(c, opens));
    EXPECT_EQ(cache.size(), 2U);
    EXPECT_EQ(opens, 3);
    cache.get(a, opener(a, opens));  // still open
    EXPECT_EQ(opens, 3);
    cache.get(b, opener(b, opens));  // was closed
    EXPECT_EQ(opens, 4);
}

TEST(IndexCache, ForgetsFilesThatAreNotCurrent) {
    const TempDir dir;
    const std::filesystem::path a = writeIndex(dir, "a.idx");
    const std::filesystem::path b = writeIndex(dir, "b.idx");
    IndexCache cache(4);
    int opens = 0;
    const std::shared_ptr<ShardReader> inUse = cache.get(a, opener(a, opens));
    cache.get(b, opener(b, opens));
    cache.forgetOthers({b});
    EXPECT_EQ(cache.size(), 1U);
    // b is still open, and a is opened again.
    cache.get(b, opener(b, opens));
    EXPECT_EQ(opens, 2);
    const std::shared_ptr<ShardReader> reopened = cache.get(a, opener(a, opens));
    EXPECT_EQ(opens, 3);
    EXPECT_NE(reopened, inUse);
    // A search still using the old file can go on reading it.
    EXPECT_EQ(inUse->docCount(), 1U);
}

TEST(IndexCache, KeepsNothingWhenOpeningFails) {
    const TempDir dir;
    IndexCache cache(4);
    EXPECT_THROW(cache.get(dir.file("missing.idx"),
                           []() -> std::unique_ptr<ShardReader> {
                               throw std::runtime_error("no such file");
                           }),
                 std::runtime_error);
    EXPECT_EQ(cache.size(), 0U);
}

TEST(IndexCache, NeedsRoomForOneFile) {
    EXPECT_THROW(IndexCache(0), std::invalid_argument);
}

}  // namespace
}  // namespace codesearch
