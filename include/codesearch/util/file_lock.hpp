#pragma once

#include <filesystem>
#include <functional>

namespace codesearch {

// Holds an exclusive lock on a file for as long as the FileLock exists. If
// another FileLock (in this or any other process) holds it, the constructor
// calls `onWait` once and then waits until the lock is free.
//
// The lock is released when the FileLock is destroyed, and also by the
// system if the process dies, so a killed process never leaves it stuck.
// Throws std::runtime_error if the lock file can't be opened.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path,
                      const std::function<void()>& onWait = nullptr);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) = delete;
    FileLock& operator=(FileLock&&) = delete;

private:
    int fd_ = -1;
};

}  // namespace codesearch
