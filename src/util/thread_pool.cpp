#include "codesearch/util/thread_pool.hpp"

#include <algorithm>
#include <utility>

namespace codesearch {

ThreadPool::ThreadPool(std::size_t threads) {
    threads = std::max<std::size_t>(threads, 1);
    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this] {
            work();
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        const std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void ThreadPool::work() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            // wait() lets go of the lock while sleeping, and checks the
            // condition again each time it wakes up.
            wake_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (tasks_.empty()) {
                return;  // stopping, and nothing left to run
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

std::size_t defaultThreadCount() {
    // hardware_concurrency can be 0 if it isn't known.
    const unsigned cores = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(cores, 1, 8);
}

}  // namespace codesearch
