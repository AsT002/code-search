#include "codesearch/git/git_runner.hpp"

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

GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args) {
    return runProcess("git", gitArgs(dir, args));
}

}  // namespace codesearch
