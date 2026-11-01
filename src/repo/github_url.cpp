#include "codesearch/repo/github_url.hpp"

#include "codesearch/util/ascii.hpp"

namespace codesearch {

namespace {

constexpr std::string_view kPrefix = "https://github.com/";
constexpr std::size_t kMaxOwnerLength = 39;  // GitHub's limits
constexpr std::size_t kMaxNameLength = 100;

bool validOwner(std::string_view owner) {
    if (owner.empty() || owner.size() > kMaxOwnerLength || owner.front() == '-') {
        return false;
    }
    for (const char c : owner) {
        if (!isAsciiAlnum(c) && c != '-') {
            return false;
        }
    }
    return true;
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        if (!isAsciiAlnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

// True if `name` ends with ".git", in any case.
bool endsWithDotGit(std::string_view name) {
    constexpr std::string_view kDotGit = ".git";
    if (name.size() < kDotGit.size()) {
        return false;
    }
    const std::string_view end = name.substr(name.size() - kDotGit.size());
    for (std::size_t i = 0; i < kDotGit.size(); ++i) {
        if (foldAscii(end[i]) != kDotGit[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<GitHubRepo> parseGitHubUrl(std::string_view text) {
    if (!text.starts_with(kPrefix)) {
        return std::nullopt;
    }
    text.remove_prefix(kPrefix.size());
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view owner = text.substr(0, slash);
    std::string_view name = text.substr(slash + 1);
    if (endsWithDotGit(name)) {
        name.remove_suffix(4);
    }
    // GitHub never keeps ".git" at the end of a repo's name, so r.git.git
    // could only be another way to write r: refuse it, or the same repo
    // could be added twice.
    if (!validOwner(owner) || !validName(name) || endsWithDotGit(name)) {
        return std::nullopt;  // this also rejects a second '/', since names can't hold one
    }
    return GitHubRepo{.owner = std::string(owner), .name = std::string(name)};
}

}  // namespace codesearch
