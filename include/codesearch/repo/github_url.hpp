#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codesearch {

struct GitHubRepo {
    std::string owner;
    std::string name;

    // The URL in one standard form: https://github.com/<owner>/<name>
    std::string url() const { return "https://github.com/" + owner + "/" + name; }
};

// Accepts only https://github.com/<owner>/<name>, with or without ".git" at
// the end. Owners use letters, digits and '-'. Names use letters, digits,
// '.', '_' and '-'. Anything else returns nothing: no other hosts, schemes,
// ports, users, query strings or extra path parts. Git reads options and
// special URL schemes from its arguments, so we pass it only URLs we know.
std::optional<GitHubRepo> parseGitHubUrl(std::string_view text);

}  // namespace codesearch
