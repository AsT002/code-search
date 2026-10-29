#include "codesearch/repo/github_url.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace codesearch {
namespace {

TEST(GitHubUrl, AcceptsOwnerAndName) {
    for (const std::string url :
         {"https://github.com/example/frontend", "https://github.com/example/frontend.git"}) {
        const std::optional<GitHubRepo> repo = parseGitHubUrl(url);
        ASSERT_TRUE(repo.has_value()) << url;
        EXPECT_EQ(repo->owner, "example");
        EXPECT_EQ(repo->name, "frontend");
        EXPECT_EQ(repo->url(), "https://github.com/example/frontend");
    }
    const std::optional<GitHubRepo> dots = parseGitHubUrl("https://github.com/a-b/x.y_z-1");
    ASSERT_TRUE(dots.has_value());
    EXPECT_EQ(dots->name, "x.y_z-1");
}

TEST(GitHubUrl, RejectsEverythingElse) {
    const std::vector<std::string> bad = {
        "",
        "github.com/a/b",
        "http://github.com/a/b",
        "https://gitlab.com/a/b",
        "https://github.com.evil.com/a/b",
        "https://user@github.com/a/b",
        "https://github.com:443/a/b",
        "HTTPS://github.com/a/b",
        "https://github.com/a",
        "https://github.com/a/",
        "https://github.com//b",
        "https://github.com/a/b/",
        "https://github.com/a/b/c",
        "https://github.com/a/b?x=1",
        "https://github.com/a/b#x",
        "https://github.com/a/.git",
        "https://github.com/a/..",
        "https://github.com/a/.",
        "https://github.com/-a/b",
        "https://github.com/a_b/c",
        "https://github.com/a/b c",
        "https://github.com/a/b\n",
        "https://github.com/a/--upload-pack=x",
        "ext::sh -c touch% /tmp/pwned",
        "file:///etc",
    };
    for (const std::string& url : bad) {
        EXPECT_FALSE(parseGitHubUrl(url).has_value()) << url;
    }
}

}  // namespace
}  // namespace codesearch
