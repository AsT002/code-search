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

// How Git finds the repo for a folder.
enum class FindRepo {
    // `git -C <dir>`: `dir` can be any folder in a repo, or a bare repo.
    FromFolder,
    // `git --git-dir=<dir>`: `dir` must be the bare repo itself. Use it for
    // clones code-search made: Git set to safe.bareRepository=explicit
    // refuses to find a bare repo with -C.
    BareRepo,
};

// The arguments for `git -C <dir> <args...>`, or with BareRepo,
// `git --git-dir=<dir> <args...>`.
std::vector<std::string> gitArgs(const std::filesystem::path& dir,
                                 const std::vector<std::string>& args,
                                 FindRepo find = FindRepo::FromFolder);

// The `extraEnv` to start Git with (see Process): `extraEnv` itself, plus
// leaving out the variables like GIT_DIR that would make Git ignore -C and
// --git-dir. Every Git we start must use this.
std::vector<std::string> gitEnv(const std::vector<std::string>& extraEnv = {});

// Runs Git with gitArgs(dir, args, find), waits for it and returns
// everything it printed. Throws GitError if git can't be started.
GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args,
                 FindRepo find = FindRepo::FromFolder);

// Git's error output, without blank lines and its "Cloning into ..."
// progress line, for adding to our own error messages.
std::string errorLines(const std::string& err);

// What to add to a "not a Git repository" message when Git fails to open a
// repo: ":\n" and Git's error lines, or nothing if Git only said there's no
// repo there. Git can refuse a repo that's there, like one owned by another
// user (safe.directory), and its message says why.
std::string notARepoReason(const std::string& err);

// The canonical top folder of the repo `dir` is in: the folder with .git in
// it, or the repo folder itself for a bare repo. Throws GitError if `dir`
// isn't in a repo, or is inside a .git folder.
std::filesystem::path repoTopFolder(const std::filesystem::path& dir);

// Makes a bare, shallow clone of `url`'s default branch in `dest`, with no
// tags: git clone --bare --depth 1 --single-branch --no-tags. Git is told
// never to ask for a password, so a private or missing repo fails instead of
// waiting, and to stop if the connection stalls for a minute. Throws
// GitError on failure. Only pass URLs you've checked.
void cloneShallow(const std::string& url, const std::filesystem::path& dest);

// Brings a clone made by cloneShallow up to date: fetches the commit the
// remote's default branch points to now (depth 1, no tags), and points the
// clone's HEAD branch at it. Works after a force-push too. Throws GitError on
// failure, or if Git would write outside `clone` (a "commondir" file). Only
// use it on clones we own, after checking they aren't symlinks
// (Workspace::checkClone).
void fetchLatest(const std::filesystem::path& clone);

}  // namespace codesearch
