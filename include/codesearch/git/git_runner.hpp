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

// Runs `git -C <dir> <args...>`, waits for it and returns everything it
// printed. Throws GitError if git can't be started.
GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args);

// Makes a bare, shallow clone of `url`'s default branch in `dest`, with no
// tags: git clone --bare --depth 1 --single-branch --no-tags. Git is told
// never to ask for a password, so a private or missing repo fails instead of
// waiting. Throws GitError on failure. Only pass URLs you've checked.
void cloneShallow(const std::string& url, const std::filesystem::path& dest);

// Brings a clone made by cloneShallow up to date: fetches the commit the
// remote's default branch points to now (depth 1, no tags), and points the
// clone's HEAD branch at it. Works after a force-push too. Throws GitError on
// failure. Only use it on clones we own.
void fetchLatest(const std::filesystem::path& clone);

}  // namespace codesearch
