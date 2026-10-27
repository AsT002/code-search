#include "codesearch/index/file_filter.hpp"

#include <fnmatch.h>

#include "codesearch/util/utf8.hpp"

#include <utility>

namespace codesearch {

std::uint64_t configFingerprint(const FilterConfig& config) {
    // Write every setting into one string, with lengths so that different
    // settings can't produce the same text...
    std::string text = "maxFileBytes " + std::to_string(config.maxFileBytes) + "\n";
    for (const std::string& pattern : config.excludePatterns) {
        text += "exclude " + std::to_string(pattern.size()) + " " + pattern + "\n";
    }

    // ...then hash it with FNV-1a, a simple, well-known 64-bit hash.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string_view skipReasonName(SkipReason reason) {
    switch (reason) {
        case SkipReason::Symlink:
            return "symlink";
        case SkipReason::Submodule:
            return "submodule";
        case SkipReason::LfsPointer:
            return "lfs pointer";
        case SkipReason::Binary:
            return "binary";
        case SkipReason::TooLarge:
            return "too large";
        case SkipReason::Excluded:
            return "excluded";
    }
    return "unknown";
}

FileFilter::FileFilter(FilterConfig config) : config_(std::move(config)) {}

std::optional<SkipReason> FileFilter::checkEntry(const std::string& path,
                                                 std::uint64_t size) const {
    for (const std::string& pattern : config_.excludePatterns) {
        // fnmatch is the POSIX function for shell-style patterns. 0 means match.
        if (::fnmatch(pattern.c_str(), path.c_str(), 0) == 0) {
            return SkipReason::Excluded;
        }
    }
    if (size > config_.maxFileBytes) {
        return SkipReason::TooLarge;
    }
    return std::nullopt;
}

std::optional<SkipReason> FileFilter::checkContent(std::string_view content) {
    if (isLfsPointer(content)) {
        return SkipReason::LfsPointer;
    }
    if (looksBinary(content)) {
        return SkipReason::Binary;
    }
    return std::nullopt;
}

bool looksBinary(std::string_view content) {
    const std::string_view start = content.substr(0, 8192);
    return start.find('\0') != std::string_view::npos;
}

namespace {

bool isDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool isLowerHex(std::string_view text) {
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'a' && c <= 'f';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

// Keys are made of lowercase letters, digits, '.' and '-'.
bool isLfsKey(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Splits "key value" at the first space. Returns false if it isn't that shape:
// exactly one space, a valid key, and no carriage return in the value.
bool splitLfsLine(std::string_view line, std::string_view& key, std::string_view& value) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return false;
    }
    key = line.substr(0, space);
    value = line.substr(space + 1);
    if (value.starts_with(' ') || value.find('\r') != std::string_view::npos) {
        return false;
    }
    return isLfsKey(key);
}

// A size in bytes. No leading zeros, since each pointer has exactly one
// valid way to be written.
bool isLfsSize(std::string_view value) {
    return isDigits(value) && (value == "0" || !value.starts_with('0'));
}

// The current version, and the older name the spec still accepts.
bool isLfsVersion(std::string_view value) {
    return value == "https://git-lfs.github.com/spec/v1" ||
           value == "https://hawser.github.com/spec/v1";
}

bool isLfsOid(std::string_view value) {
    const std::string_view prefix = "sha256:";
    if (!value.starts_with(prefix)) {
        return false;
    }
    const std::string_view hash = value.substr(prefix.size());
    return hash.size() == 64 && isLowerHex(hash);
}

}  // namespace

bool isLfsPointer(std::string_view content) {
    // Follows the pointer format in the Git LFS spec
    // (https://github.com/git-lfs/git-lfs/blob/main/docs/spec.md#the-pointer):
    // - under 1024 bytes, valid UTF-8, made only of "key value\n" lines
    // - "version" comes first, the other keys follow in sorted order
    // - one space between key and value; values have no carriage returns
    // - "oid" is "sha256:" plus 64 hex characters, "size" is a number
    if (content.size() >= 1024 || content.empty() || content.back() != '\n' ||
        !isValidUtf8(content)) {
        return false;
    }

    bool first = true;
    bool haveOid = false;
    bool haveSize = false;
    std::string_view previousKey;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t end = content.find('\n', pos);  // found: content ends with '\n'
        std::string_view key;
        std::string_view value;
        if (!splitLfsLine(content.substr(pos, end - pos), key, value)) {
            return false;
        }
        pos = end + 1;

        if (first) {
            if (key != "version" || !isLfsVersion(value)) {
                return false;
            }
            first = false;
            continue;  // "version" isn't part of the sorted keys
        }
        if (key == "version" || key <= previousKey) {
            return false;  // version only comes first; others are sorted and appear once
        }
        if (key == "oid") {
            haveOid = isLfsOid(value);
            if (!haveOid) {
                return false;
            }
        } else if (key == "size") {
            haveSize = isLfsSize(value);
            if (!haveSize) {
                return false;
            }
        }
        previousKey = key;
    }
    return haveOid && haveSize;
}

}  // namespace codesearch
