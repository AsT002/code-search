#pragma once

namespace codesearch {

// Lowercases ASCII letters and leaves every other byte alone. It never looks at
// the system locale, so non-ASCII bytes are never changed.
constexpr char foldAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// True for the ASCII letters a-z and A-Z and the digits 0-9.
constexpr bool isAsciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

}  // namespace codesearch
