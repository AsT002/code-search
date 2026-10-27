#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "codesearch/util/process.hpp"

namespace codesearch {

// Thrown when Git fails or gives an answer we don't expect.
class GitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;  // reuse runtime_error's constructors
};

using GitResult = ProcessResult;  // another name for the same type

// The arguments for `git -C <dir> <args...>`.
std::vector<std::string> gitArgs(const std::filesystem::path& dir,
                                 const std::vector<std::string>& args);

// Runs `git -C <dir> <args...>`, waits for it and returns everything it printed.
GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args);

}  // namespace codesearch
