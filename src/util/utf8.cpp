#include "codesearch/util/utf8.hpp"

#include <algorithm>

#include <cstddef>

namespace codesearch {

namespace {

// What the first byte of a character says about the rest of it.
struct LeadByte {
    std::size_t length = 0;          // bytes in the character; 0 means invalid
    unsigned char secondMin = 0x80;  // allowed range for the second byte
    unsigned char secondMax = 0xBF;
};

// Follows the table of well-formed byte sequences in the Unicode standard
// (section 3.9, table 3-7). The narrower second-byte ranges rule out
// overlong forms (E0, F0), surrogates (ED) and values above U+10FFFF (F4).
LeadByte readLead(unsigned char byte) {
    if (byte <= 0x7F) {
        return {1, 0x80, 0xBF};  // ASCII
    }
    if (byte >= 0xC2 && byte <= 0xDF) {
        return {2, 0x80, 0xBF};  // C0 and C1 would be overlong
    }
    if (byte == 0xE0) {
        return {3, 0xA0, 0xBF};
    }
    if ((byte >= 0xE1 && byte <= 0xEC) || byte == 0xEE || byte == 0xEF) {
        return {3, 0x80, 0xBF};
    }
    if (byte == 0xED) {
        return {3, 0x80, 0x9F};
    }
    if (byte == 0xF0) {
        return {4, 0x90, 0xBF};
    }
    if (byte >= 0xF1 && byte <= 0xF3) {
        return {4, 0x80, 0xBF};
    }
    if (byte == 0xF4) {
        return {4, 0x80, 0x8F};
    }
    return {};  // 80 to C1, F5 to FF: never valid as a first byte
}

// The length of the valid character starting at text[i], or 0 if there
// isn't one there.
std::size_t validLength(std::string_view text, std::size_t i) {
    const LeadByte lead = readLead(static_cast<unsigned char>(text[i]));
    if (lead.length == 0 || lead.length > text.size() - i) {
        return 0;  // bad first byte, or the text ends mid-character
    }
    for (std::size_t k = 1; k < lead.length; ++k) {
        const auto byte = static_cast<unsigned char>(text[i + k]);
        const unsigned char low = k == 1 ? lead.secondMin : 0x80;
        const unsigned char high = k == 1 ? lead.secondMax : 0xBF;
        if (byte < low || byte > high) {
            return 0;
        }
    }
    return lead.length;
}

}  // namespace

std::size_t utf8Length(std::string_view text, std::size_t at) {
    return validLength(text, at);
}

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t length = validLength(text, i);
        if (length == 0) {
            return false;
        }
        i += length;
    }
    return true;
}

namespace {

void appendHex(std::string& out, unsigned char byte) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    out += "\\x";
    out += kDigits[byte >> 4U];
    out += kDigits[byte & 0xFU];
}

}  // namespace

std::string escapeForTerminal(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const std::size_t length = validLength(text, i);
        // C1 controls are U+0080 to U+009F: C2 80 to C2 9F in UTF-8.
        const bool c1 =
            length == 2 && byte == 0xC2 && static_cast<unsigned char>(text[i + 1]) <= 0x9F;
        const bool control = length == 1 && ((byte < 0x20 && byte != '\t') || byte == 0x7F);
        if (length == 0 || control || c1) {
            for (std::size_t k = 0; k < std::max<std::size_t>(length, 1); ++k) {
                appendHex(out, static_cast<unsigned char>(text[i + k]));
            }
            i += std::max<std::size_t>(length, 1);
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

std::string replaceInvalidUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t length = validLength(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD in UTF-8
            ++i;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

}  // namespace codesearch
