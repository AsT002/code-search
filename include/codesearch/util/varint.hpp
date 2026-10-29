#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace codesearch {

// Varints store a number in 1 to 5 bytes: 7 bits per byte, lowest bits first.
// The top bit of each byte is set when another byte follows. Small numbers
// take 1 byte; the largest uint32 takes 5.

// Adds `value` to the end of `out`.
void appendVarint(std::string& out, std::uint32_t value);

// Reads the varint that starts at bytes[pos] and moves `pos` past it. Returns
// std::nullopt, leaving `pos` alone, if the bytes end mid-number or the number
// doesn't fit in 32 bits.
std::optional<std::uint32_t> readVarint(std::string_view bytes, std::size_t& pos);

}  // namespace codesearch
