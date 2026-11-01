#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <new>

#include "codesearch/util/thread_pool.hpp"

namespace codesearch::testing {

// A pool that runs the first `okSubmits` tasks it's given, then throws
// std::bad_alloc from submit, as a real pool does when memory runs out.
class FailingPool : public ThreadPool {
public:
    FailingPool(std::size_t threads, int okSubmits) : ThreadPool(threads), left_(okSubmits) {}

    void submit(std::function<void()> task) override {
        if (left_ <= 0) {
            throw std::bad_alloc();
        }
        --left_;
        ThreadPool::submit(std::move(task));
    }

private:
    int left_;  // only touched by the thread that submits
};

// Runs `fn` on another thread. If it hasn't returned after a minute, it's
// stuck, and the test can't go on: print why and stop the whole program.
template <typename Fn>
void failIfItHangs(Fn fn) {
    std::future<void> done = std::async(std::launch::async, std::move(fn));
    if (done.wait_for(std::chrono::minutes(1)) == std::future_status::timeout) {
        static_cast<void>(std::fputs("still running after a minute: it hangs\n", stderr));
        std::abort();
    }
    done.get();  // passes on what fn threw
}

}  // namespace codesearch::testing
