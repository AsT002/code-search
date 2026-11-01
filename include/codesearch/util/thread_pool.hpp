#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace codesearch {

// A fixed number of threads that run tasks from one shared queue. Searches
// and builds share one pool, so the number of threads busy at once stays
// limited however many repos or requests there are.
//
// Tasks must not throw; catch inside the task and pass the error back.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads);
    // Runs every task still in the queue, then stops the threads.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    std::size_t size() const { return threads_.size(); }

    // Adds a task to the queue. Some thread runs it later.
    void submit(std::function<void()> task);

private:
    void work();

    std::mutex mutex_;
    std::condition_variable wake_;  // signaled when a task arrives or we stop
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

// How many threads a pool should have on this machine: one per core, at
// most 8. More than that rarely helps, since reading memory becomes the limit.
std::size_t defaultThreadCount();

}  // namespace codesearch
