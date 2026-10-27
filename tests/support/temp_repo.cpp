#include "temp_repo.hpp"

#include <unistd.h>  // mkdtemp on macOS

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace codesearch::testing {

TempRepo::TempRepo(const std::string& objectFormat) {
    // mkdtemp replaces the XXXXXX with random characters and creates the folder.
    std::string pattern =
        (std::filesystem::temp_directory_path() / "code-search-test-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a temp folder");
    }
    dir_ = pattern;
    git({"init", "--quiet", "--initial-branch=main", "--object-format=" + objectFormat});
}

TempRepo::~TempRepo() {
    std::error_code error;  // ignore errors; a leftover temp folder is harmless
    std::filesystem::remove_all(dir_, error);
}

void TempRepo::writeUnstaged(const std::string& path, std::string_view content) {
    const std::filesystem::path full = dir_ / path;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        throw std::runtime_error("could not write " + full.string());
    }
}

void TempRepo::write(const std::string& path, std::string_view content) {
    writeUnstaged(path, content);
    git({"add", "--", path});
}

void TempRepo::symlink(const std::string& path, const std::string& target) {
    std::filesystem::create_symlink(target, dir_ / path);
    git({"add", "--", path});
}

void TempRepo::addSubmodule(const std::string& path, const ObjectId& commit) {
    // Mode 160000 is how Git records a submodule.
    git({"update-index", "--add", "--cacheinfo", "160000," + commit.hex() + "," + path});
}

namespace {

ObjectId parseId(const std::string& out) {
    const std::optional<ObjectId> id = ObjectId::fromHex(out.substr(0, 40));
    if (!id.has_value()) {
        throw std::runtime_error("unexpected object ID: " + out);
    }
    return id.value();
}

}  // namespace

ObjectId TempRepo::commit(const std::string& message) {
    git({"commit", "--quiet", "--allow-empty", "-m", message});
    return parseId(git({"rev-parse", "HEAD"}).out);
}

ObjectId TempRepo::blobId(const std::string& path) {
    return parseId(git({"rev-parse", "HEAD:" + path}).out);
}

void TempRepo::removeObject(const ObjectId& id) {
    // Loose objects live at .git/objects/<first 2 hex chars>/<other 38>.
    const std::filesystem::path file =
        dir_ / ".git" / "objects" / id.hex().substr(0, 2) / id.hex().substr(2);
    if (!std::filesystem::remove(file)) {
        throw std::runtime_error("no such object file: " + file.string());
    }
}

GitResult TempRepo::git(const std::vector<std::string>& args) {
    std::vector<std::string> fullArgs = {
        "-c", "user.name=Test",       "-c", "user.email=test@example.com",
        "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
        "-c", "core.autocrlf=false",
    };
    for (const std::string& arg : args) {
        fullArgs.push_back(arg);
    }
    GitResult result = runGit(dir_, fullArgs);
    if (result.exitCode != 0) {
        throw std::runtime_error("git " + args.front() + " failed: " + result.err);
    }
    return result;
}

}  // namespace codesearch::testing
