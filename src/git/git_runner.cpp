#include "codesearch/git/git_runner.hpp"

#include <string_view>

namespace codesearch {

std::vector<std::string> gitArgs(const std::filesystem::path& dir,
                                 const std::vector<std::string>& args) {
    std::vector<std::string> result = {"-C", dir.string()};
    result.reserve(2 + args.size());
    for (const std::string& arg : args) {
        result.push_back(arg);
    }
    return result;
}

namespace {

GitResult runGitProcess(const std::vector<std::string>& args,
                        const std::vector<std::string>& extraEnv = {}) {
    try {
        return runProcess("git", args, extraEnv);
    } catch (const ProcessStartError& error) {
        throw GitError(std::string(error.what()) + ". Is Git installed and on your PATH?");
    }
}

// Git's error messages, without blank lines and its "Cloning into ..." progress line.
std::string errorLines(const std::string& err) {
    std::string message;
    std::size_t start = 0;
    while (start < err.size()) {
        std::size_t end = err.find('\n', start);
        if (end == std::string::npos) {
            end = err.size();
        }
        const std::string_view line = std::string_view(err).substr(start, end - start);
        if (!line.empty() && !line.starts_with("Cloning into")) {
            message += message.empty() ? "" : "\n";
            message += line;
        }
        start = end + 1;
    }
    return message;
}

}  // namespace

GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args) {
    return runGitProcess(gitArgs(dir, args));
}

void cloneShallow(const std::string& url, const std::filesystem::path& dest) {
    const std::vector<std::string> args = {
        "clone", "--bare", "--depth", "1", "--single-branch", "--no-tags", "--", url, dest.string(),
    };
    const GitResult result = runGitProcess(args, {"GIT_TERMINAL_PROMPT=0"});
    if (result.exitCode != 0) {
        throw GitError("could not clone " + url + ":\n" + errorLines(result.err));
    }
}

void fetchLatest(const std::filesystem::path& clone) {
    // --git-dir instead of -C: Git uses exactly this folder, and never looks
    // in the folders above it for another repo if this one is broken.
    const std::string gitDir = "--git-dir=" + clone.string();
    const GitResult fetched =
        runGitProcess({gitDir, "fetch", "--quiet", "--depth", "1", "--no-tags", "origin", "HEAD"},
                      {"GIT_TERMINAL_PROMPT=0"});
    if (fetched.exitCode != 0) {
        throw GitError("could not fetch into " + clone.string() + ":\n" + errorLines(fetched.err));
    }
    // fetch saved the commit as FETCH_HEAD. Move our branch to it.
    const GitResult moved = runGitProcess({gitDir, "update-ref", "HEAD", "FETCH_HEAD"});
    if (moved.exitCode != 0) {
        throw GitError("could not update " + clone.string() + ":\n" + errorLines(moved.err));
    }
}

}  // namespace codesearch
