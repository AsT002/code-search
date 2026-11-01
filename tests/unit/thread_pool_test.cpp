#include "codesearch/util/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <system_error>

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

// macOS lets one process have 4096 threads, so asking for more makes
// starting one fail partway. The threads already started must be stopped
// before the error goes on; destroying one that's still running would end
// the whole program.
TEST(ThreadPool, TooManyThreadsIsAnError) {
#ifndef __APPLE__
    GTEST_SKIP() << "relies on macOS's limit of 4096 threads per process";
#elif defined(__has_feature) && __has_feature(thread_sanitizer)
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
#if defined(__APPLE__) && !(defined(__has_feature) && __has_feature(thread_sanitizer))
    EXPECT_EQ(startPool(100000), nullptr);  // over macOS's 4096 threads per process
#endif
}

}  // namespace
}  // namespace codesearch
