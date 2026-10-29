#include "temp_dir.hpp"

#include <unistd.h>  // mkdtemp on macOS

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace codesearch::testing {

TempDir::TempDir() {
    // mkdtemp replaces the XXXXXX with random characters and creates the folder.
    std::string pattern =
        (std::filesystem::temp_directory_path() / "code-search-test-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a temp folder");
    }
    dir_ = pattern;
}

TempDir::~TempDir() {
    std::error_code error;  // ignore errors; a leftover temp folder is harmless
    std::filesystem::remove_all(dir_, error);
}

std::string readFile(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    if (!in) {
        throw std::runtime_error("could not read " + path.string());
    }
    return bytes.str();
}

void writeFile(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("could not write " + path.string());
    }
}

}  // namespace codesearch::testing
