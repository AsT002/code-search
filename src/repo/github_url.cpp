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
    if (name.ends_with(".git")) {
        name.remove_suffix(4);
    }
    if (!validOwner(owner) || !validName(name)) {
        return std::nullopt;  // this also rejects a second '/', since names can't hold one
    }
    return GitHubRepo{.owner = std::string(owner), .name = std::string(name)};
}

}  // namespace codesearch
