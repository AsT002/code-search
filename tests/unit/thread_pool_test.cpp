#include "codesearch/util/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>

// Defined when built with ThreadSanitizer, which can't track thousands of
// threads. GCC says so with __SANITIZE_THREAD__, Clang with __has_feature.
// GCC has no __has_feature, so it gets its own #if: even behind a false
// defined() check, GCC would fail to read it.
#ifdef __SANITIZE_THREAD__
#define CODESEARCH_UNDER_TSAN
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CODESEARCH_UNDER_TSAN
#endif
#endif

namespace codesearch {
namespace {

TEST(ThreadPool, RunsEveryTaskBeforeStopping) {
    std::atomic<int> ran = 0;
    {
        ThreadPool pool(3);
        for (int i = 0; i < 100; ++i) {
            pool.submit([&] {
                ++ran;
            });
        }
    }
    EXPECT_EQ(ran.load(), 100);
}

// Each of a pool's threads has its own number, and threads that aren't
// the pool's, like the test's or another pool's, have none.
TEST(ThreadPool, EachThreadKnowsItsNumber) {
    // Before the pools, so they're destroyed after them: the pools' threads
    // may still be using these until the pools stop and join them.
    std::mutex mutex;
    std::condition_variable changed;
    std::set<std::size_t> seen;
    bool otherSawNone = true;
    int waiting = 0;

    ThreadPool pool(3);
    const ThreadPool other(2);
    EXPECT_FALSE(pool.workerIndex().has_value());
    // Three tasks that wait for each other, so each runs on its own thread.
    for (int i = 0; i < 3; ++i) {
        pool.submit([&] {
            std::unique_lock lock(mutex);
            const std::optional<std::size_t> index = pool.workerIndex();
            if (index.has_value()) {
                seen.insert(index.value());
            }
            otherSawNone = otherSawNone && !other.workerIndex().has_value();
            ++waiting;
            changed.notify_all();
            changed.wait(lock, [&] {
                return waiting == 3;
            });
        });
    }
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] {
        return waiting == 3;
    });
    EXPECT_EQ(seen, (std::set<std::size_t>{0, 1, 2}));
    lock.unlock();
    EXPECT_TRUE(otherSawNone);
}

// macOS lets one process have 4096 threads, so asking for more makes
// starting one fail partway. The threads already started must be stopped
// before the error goes on; destroying one that's still running would end
// the whole program.
TEST(ThreadPool, TooManyThreadsIsAnError) {
#ifndef __APPLE__
    GTEST_SKIP() << "relies on macOS's limit of 4096 threads per process";
#elif defined(CODESEARCH_UNDER_TSAN)
    GTEST_SKIP() << "ThreadSanitizer can't track this many threads";
#else
    EXPECT_THROW(ThreadPool(100000), std::system_error);
#endif
}

// Search and build fall back to the calling thread rather than fail.
TEST(StartPool, GivesNoPoolForOneThreadOrWhenThreadsRunOut) {
    EXPECT_EQ(startPool(1), nullptr);
    const std::unique_ptr<ThreadPool> pool = startPool(3);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->size(), 3U);
#if defined(__APPLE__) && !defined(CODESEARCH_UNDER_TSAN)
    EXPECT_EQ(startPool(100000), nullptr);  // over macOS's 4096 threads per process
#endif
}

}  // namespace
}  // namespace codesearch
