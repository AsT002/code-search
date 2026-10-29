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
        // Keep git's messages, minus its "Cloning into ..." progress line.
        std::string message;
        std::size_t start = 0;
        while (start < result.err.size()) {
            std::size_t end = result.err.find('\n', start);
            if (end == std::string::npos) {
                end = result.err.size();
            }
            const std::string_view line = std::string_view(result.err).substr(start, end - start);
            if (!line.empty() && !line.starts_with("Cloning into")) {
                message += message.empty() ? "" : "\n";
                message += line;
            }
            start = end + 1;
        }
        throw GitError("could not clone " + url + ":\n" + message);
    }
}

}  // namespace codesearch
