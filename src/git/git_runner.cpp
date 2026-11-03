#include "codesearch/git/git_runner.hpp"

#include <array>
#include <string_view>

namespace codesearch {

std::vector<std::string> gitArgs(const std::filesystem::path& dir,
                                 const std::vector<std::string>& args, FindRepo find) {
    std::vector<std::string> result;
    result.reserve(2 + args.size());
    if (find == FindRepo::BareRepo) {
        result.push_back("--git-dir=" + dir.string());
    } else {
        result.push_back("-C");
        result.push_back(dir.string());
    }
    for (const std::string& arg : args) {
        result.push_back(arg);
    }
    return result;
}

namespace {

// Variables that tell Git which repo to use, and how, from
// `git rev-parse --local-env-vars`. Git sets them for hooks and aliases, so
// code-search run from one of those would get them too, and then -C and
// --git-dir would be ignored: Git would read or write the repo they name.
// They're left out of every Git we start.
constexpr std::array<std::string_view, 15> kRepoEnvVars = {
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_COMMON_DIR",
    "GIT_CONFIG",
    "GIT_CONFIG_COUNT",
    "GIT_CONFIG_PARAMETERS",
    "GIT_DIR",
    "GIT_GRAFT_FILE",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_NO_REPLACE_OBJECTS",
    "GIT_OBJECT_DIRECTORY",
    "GIT_PREFIX",
    "GIT_REPLACE_REF_BASE",
    "GIT_SHALLOW_FILE",
    "GIT_WORK_TREE",
};

}  // namespace

std::vector<std::string> gitEnv(const std::vector<std::string>& extraEnv) {
    // Plain names, so the Process leaves them out.
    std::vector<std::string> env(kRepoEnvVars.begin(), kRepoEnvVars.end());
    // Git reads "replacement" objects (`git replace`) in place of the real
    // ones. An index is saved under its commit's id, and replacements can
    // change without that id changing, so an index would go stale with no
    // way to tell. Read the objects the id names, always.
    env.emplace_back("GIT_NO_REPLACE_OBJECTS=1");
    env.insert(env.end(), extraEnv.begin(), extraEnv.end());
    return env;
}

namespace {

// The extra environment for Git commands that use the network.
// GIT_TERMINAL_PROMPT=0: never ask for a password in the terminal. Git asks
// a GIT_ASKPASS, core.askPass or SSH_ASKPASS program before it checks that,
// and editors like VS Code set GIT_ASKPASS, so an empty GIT_ASKPASS turns
// those off too, and GCM_INTERACTIVE=never stops Git Credential Manager's
// window. A private or missing repo then fails instead of waiting.
// GIT_HTTP_LOW_SPEED_*: stop if fewer than 1,000 bytes a second arrive for
// 60 seconds, so a stalled connection fails instead of holding the
// workspace lock forever.
std::vector<std::string> networkEnv() {
    return {
        "GIT_TERMINAL_PROMPT=0",
        "GIT_ASKPASS=",
        "SSH_ASKPASS=",
        "GCM_INTERACTIVE=never",
        "GIT_HTTP_LOW_SPEED_LIMIT=1000",
        "GIT_HTTP_LOW_SPEED_TIME=60",
    };
}

GitResult runGitProcess(const std::vector<std::string>& args,
                        const std::vector<std::string>& extraEnv = {}) {
    try {
        return runProcess("git", args, gitEnv(extraEnv));
    } catch (const ProcessStartError& error) {
        throw GitError(std::string(error.what()) + ". Is Git installed and on your PATH?");
    }
}

// `text` without the newline Git adds at the end. Only that one: a path can
// hold newlines, since a folder name may.
std::string withoutFinalNewline(std::string text) {
    if (text.ends_with('\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

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

std::string notARepoReason(const std::string& err) {
    const std::string lines = errorLines(err);
    if (lines.empty() || lines.find("not a git repository") != std::string::npos) {
        return "";
    }
    return ":\n" + lines;
}

GitResult runGit(const std::filesystem::path& dir, const std::vector<std::string>& args,
                 FindRepo find) {
    return runGitProcess(gitArgs(dir, args, find));
}

std::filesystem::path repoTopFolder(const std::filesystem::path& dir) {
    const GitResult bare = runGit(dir, {"rev-parse", "--is-bare-repository"});
    if (bare.exitCode != 0) {
        throw GitError("not a Git repository: " + dir.string() + notARepoReason(bare.err));
    }
    // A bare repo has no work tree, so its top folder is the repo folder itself.
    const bool isBare = withoutFinalNewline(bare.out) == "true";
    const GitResult top =
        runGit(dir, {"rev-parse", isBare ? "--absolute-git-dir" : "--show-toplevel"});
    if (top.exitCode != 0) {
        // The usual reason: `dir` is a repo's .git folder, or inside it.
        throw GitError(dir.string() +
                       " is inside a repo's .git folder. Use the repo's own folder instead");
    }
    std::error_code error;
    std::filesystem::path path = std::filesystem::canonical(withoutFinalNewline(top.out), error);
    if (error) {
        throw GitError("can't find the top folder of the repo at " + dir.string());
    }
    return path;
}

void cloneShallow(const std::string& url, const std::filesystem::path& dest) {
    const std::vector<std::string> args = {
        "clone", "--bare", "--depth", "1", "--single-branch", "--no-tags", "--", url, dest.string(),
    };
    const GitResult result = runGitProcess(args, networkEnv());
    if (result.exitCode != 0) {
        throw GitError("could not clone " + url + ":\n" + errorLines(result.err));
    }
}

void fetchLatest(const std::filesystem::path& clone) {
    // --git-dir instead of -C: Git uses exactly this folder, and never looks
    // in the folders above it for another repo if this one is broken.
    const std::string gitDir = "--git-dir=" + clone.string();

    // A "commondir" file in a repo folder tells Git to keep its branches in
    // another folder. Ask Git where it would write, and check that it's here.
    const GitResult where = runGitProcess({gitDir, "rev-parse", "--git-common-dir"});
    if (where.exitCode != 0) {
        throw GitError(clone.string() + " isn't a Git repository:\n" + errorLines(where.err));
    }
    std::error_code error;
    const std::filesystem::path commonDir =
        std::filesystem::canonical(withoutFinalNewline(where.out), error);
    if (error || commonDir != std::filesystem::canonical(clone)) {
        throw GitError("refusing to update " + clone.string() +
                       ": Git would write to another folder");
    }
    const GitResult fetched = runGitProcess(
        {gitDir, "fetch", "--quiet", "--depth", "1", "--no-tags", "origin", "HEAD"}, networkEnv());
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
