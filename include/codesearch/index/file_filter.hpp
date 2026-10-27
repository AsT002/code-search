#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codesearch {

// Goes up by one whenever we change what the filter code does, so old indexes
// get rebuilt with the new rules.
inline constexpr std::uint32_t kPolicyVersion = 1;

// Settings the user can change.
struct FilterConfig {
    std::uint64_t maxFileBytes = 1024ULL * 1024;  // 1 MB
    // Shell-style patterns matched against the whole path. "*" also matches "/",
    // so "*.min.js" matches "a/b/app.min.js".
    std::vector<std::string> excludePatterns = {"*.min.js", "*.min.css", "*.js.map", "*.css.map"};
};

// A hash of every setting in `config`. If any setting changes, so does this.
std::uint64_t configFingerprint(const FilterConfig& config);

enum class SkipReason {
    Symlink,
    Submodule,
    LfsPointer,
    Binary,
    TooLarge,
    Excluded,
};

// A short name for reports, like "too large".
std::string_view skipReasonName(SkipReason reason);

// Decides which files get indexed.
class FileFilter {
public:
    explicit FileFilter(FilterConfig config = {});

    const FilterConfig& config() const { return config_; }

    // Checks what we know before reading the file. Returns why to skip it, or
    // nothing to read it.
    std::optional<SkipReason> checkEntry(const std::string& path, std::uint64_t size) const;

    // Checks the content after reading it.
    static std::optional<SkipReason> checkContent(std::string_view content);

private:
    FilterConfig config_;
};

// A NUL byte in the first 8 KB means binary. Only a guess, so everything else
// still has to work if a binary file gets through.
bool looksBinary(std::string_view content);

// A Git LFS pointer is a small text file that stands in for a large file
// stored elsewhere. Indexing it would show the pointer as if it were the file.
bool isLfsPointer(std::string_view content);

}  // namespace codesearch
