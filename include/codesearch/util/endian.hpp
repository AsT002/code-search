#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace codesearch {

// Fixed-size little-endian numbers: the lowest byte comes first. Index files
// use these for every number, so a file reads the same on any machine. We
// build numbers byte by byte instead of casting raw memory to an integer,
// which C++ doesn't allow for bytes that came from a file.

// Adds the low `size` bytes of `value` to `out`, lowest byte first.
inline void appendLittle(std::string& out, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>((value >> (8U * i)) & 0xFFU));
    }
}

inline void appendU8(std::string& out, std::uint8_t value) {
    appendLittle(out, value, 1);
}
inline void appendU16(std::string& out, std::uint16_t value) {
    appendLittle(out, value, 2);
}
inline void appendU32(std::string& out, std::uint32_t value) {
    appendLittle(out, value, 4);
}
inline void appendU64(std::string& out, std::uint64_t value) {
    appendLittle(out, value, 8);
}

// Reads `size` bytes at `offset` as a little-endian number. Throws
// std::out_of_range if they go past the end. Callers check ranges first, so
// this only fires on a bug in our own checks.
inline std::uint64_t readLittle(std::string_view bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw std::out_of_range("read past the end of the bytes");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (8U * i);
    }
    return value;
}

inline std::uint8_t readU8(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(readLittle(bytes, offset, 1));
}
inline std::uint16_t readU16(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(readLittle(bytes, offset, 2));
}
inline std::uint32_t readU32(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(readLittle(bytes, offset, 4));
}
inline std::uint64_t readU64(std::string_view bytes, std::size_t offset) {
    return readLittle(bytes, offset, 8);
}

}  // namespace codesearch
