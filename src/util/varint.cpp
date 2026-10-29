#include "codesearch/util/varint.hpp"

namespace codesearch {

namespace {

constexpr unsigned char kMoreBit = 0x80;    // "another byte follows"
constexpr unsigned char kValueBits = 0x7F;  // the 7 bits of the number
constexpr std::size_t kMaxBytes = 5;        // 5 × 7 = 35 bits, enough for 32

}  // namespace

void appendVarint(std::string& out, std::uint32_t value) {
    while (value > kValueBits) {
        out.push_back(static_cast<char>((value & kValueBits) | kMoreBit));
        value >>= 7U;
    }
    out.push_back(static_cast<char>(value));
}

std::optional<std::uint32_t> readVarint(std::string_view bytes, std::size_t& pos) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < kMaxBytes; ++i) {
        if (pos + i >= bytes.size()) {
            return std::nullopt;  // ends mid-number
        }
        const auto byte = static_cast<unsigned char>(bytes[pos + i]);
        // The 5th byte holds bits 28 to 34. Only 28 to 31 fit in a uint32.
        if (i == kMaxBytes - 1 && byte > 0x0F) {
            return std::nullopt;
        }
        value |= static_cast<std::uint32_t>(byte & kValueBits) << (7U * i);
        if ((byte & kMoreBit) == 0) {
            pos += i + 1;
            return value;
        }
    }
    return std::nullopt;  // not reached: the 5th byte check above stops it
}

}  // namespace codesearch
