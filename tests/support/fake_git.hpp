#pragma once

#include <filesystem>
#include <string>

namespace codesearch::testing {

// While a FakeGit is alive, "git" means a shell script we wrote instead of
// the real Git. Lets tests feed our code broken or truncated Git output.
//
// The script gets Git's arguments with "-C <dir>" already removed, so $1 is
// the command, like "ls-tree". Start the script with kAnswerSha1 if the code
// under test opens a GitRepo first.
class FakeGit {
public:
    explicit FakeGit(const std::string& script);
    ~FakeGit();

    FakeGit(const FakeGit&) = delete;
    FakeGit& operator=(const FakeGit&) = delete;
    FakeGit(FakeGit&&) = delete;
    FakeGit& operator=(FakeGit&&) = delete;

private:
    std::filesystem::path dir_;
    std::string oldPath_;
};

// Answers the format check that GitRepo's constructor makes.
inline const char* const kAnswerSha1 =
    "if [ \"$1 $2\" = \"rev-parse --show-object-format\" ]; then echo sha1; exit 0; fi\n";

}  // namespace codesearch::testing
