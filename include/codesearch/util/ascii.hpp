#pragma once

namespace codesearch {

// Lowercases ASCII letters and leaves every other byte alone. It never looks at
// the system locale, so non-ASCII bytes are never changed.
constexpr char foldAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace codesearch
