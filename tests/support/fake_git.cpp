#include "fake_git.hpp"

#include <unistd.h>  // mkdtemp on macOS

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace codesearch::testing {

FakeGit::FakeGit(const std::string& script) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "code-search-fake-git-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a temp folder");
    }
    dir_ = pattern;

    const std::filesystem::path git = dir_ / "git";
    {
        std::ofstream out(git, std::ios::binary);
        out << "#!/bin/sh\nshift 2\n" << script;  // shift 2 drops "-C <dir>"
    }
    std::filesystem::permissions(git, std::filesystem::perms::owner_all);

    // getenv and setenv aren't safe while other threads use them. Tests that
    // use FakeGit don't start any threads of their own.
    const char* path = std::getenv("PATH");  // NOLINT(concurrency-mt-unsafe)
    oldPath_ = path == nullptr ? "" : path;
    const std::string newPath = dir_.string() + ":" + oldPath_;
    ::setenv("PATH", newPath.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
}

FakeGit::~FakeGit() {
    ::setenv("PATH", oldPath_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    std::error_code error;
    std::filesystem::remove_all(dir_, error);
}

}  // namespace codesearch::testing
