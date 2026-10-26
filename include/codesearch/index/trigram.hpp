#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace codesearch {

// Three bytes packed into the low 24 bits: (b0 << 16) | (b1 << 8) | b2.
using Trigram = std::uint32_t;

constexpr Trigram packTrigram(char a, char b, char c) {
    return (static_cast<Trigram>(static_cast<unsigned char>(a)) << 16U) |
           (static_cast<Trigram>(static_cast<unsigned char>(b)) << 8U) |
           static_cast<Trigram>(static_cast<unsigned char>(c));
}

// The distinct trigrams in `text` after ASCII case folding, sorted. Text
// shorter than three bytes has none.
std::vector<Trigram> extractTrigrams(std::string_view text);

}  // namespace codesearch
