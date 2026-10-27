#include "codesearch/util/utf8.hpp"

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

}  // namespace

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const LeadByte lead = readLead(static_cast<unsigned char>(text[i]));
        if (lead.length == 0 || lead.length > text.size() - i) {
            return false;  // bad first byte, or the text ends mid-character
        }
        for (std::size_t k = 1; k < lead.length; ++k) {
            const auto byte = static_cast<unsigned char>(text[i + k]);
            const unsigned char low = k == 1 ? lead.secondMin : 0x80;
            const unsigned char high = k == 1 ? lead.secondMax : 0xBF;
            if (byte < low || byte > high) {
                return false;
            }
        }
        i += lead.length;
    }
    return true;
}

}  // namespace codesearch
