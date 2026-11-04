#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
    // Throws std::system_error if the system can't start that many threads.
    explicit ThreadPool(std::size_t threads);
    // Runs every task still in the queue, then stops the threads.
    virtual ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    std::size_t size() const { return threads_.size(); }

    // Which of this pool's threads is calling, from 0 to size() - 1, or
    // nothing if it isn't one of them. Lets a task use things kept one per
    // thread, which then need no lock.
    std::optional<std::size_t> workerIndex() const;

    // Adds a task to the queue. Some thread runs it later. Can throw (if
    // memory runs out); then the task never runs. Virtual so tests can make
    // it fail.
    virtual void submit(std::function<void()> task);

private:
    void work(std::size_t index);
    void stopAndJoin();

    std::mutex mutex_;
    std::condition_variable wake_;  // signaled when a task arrives or we stop
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

// How many threads a pool should have on this machine: one per core, at
// most 8. More than that rarely helps, since reading memory becomes the limit.
std::size_t defaultThreadCount();

// A pool for searches and builds, or nullptr. With one thread, a pool
// would only add handing the work over. And if the system can't start the
// threads, the work can still run on the calling thread. Code that takes a
// ThreadPool* does that when given nullptr.
std::unique_ptr<ThreadPool> startPool(std::size_t threads = defaultThreadCount());

}  // namespace codesearch
