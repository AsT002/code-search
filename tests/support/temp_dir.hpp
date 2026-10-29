#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace codesearch::testing {

// A new empty temp folder, deleted with everything in it when the TempDir
// goes away.
class TempDir {
public:
    TempDir();
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    const std::filesystem::path& dir() const { return dir_; }

    // The path of `name` inside the folder.
    std::filesystem::path file(const std::string& name) const { return dir_ / name; }

private:
    std::filesystem::path dir_;
};

// Whole-file helpers. Both throw std::runtime_error on failure.
std::string readFile(const std::filesystem::path& path);
void writeFile(const std::filesystem::path& path, std::string_view bytes);

}  // namespace codesearch::testing
