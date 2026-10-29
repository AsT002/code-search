#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "codesearch/index/posting_cursor.hpp"

namespace codesearch {

// The layout of an index (.idx) file, version 1. See "Index file" in
// docs/ARCHITECTURE.md. All numbers are little-endian. The file is:
//
//   header (kHeaderBytes), then five sections in this order, each starting
//   on an 8-byte boundary: trigram table, postings, doc table, paths, contents.
//
// Offsets below are in bytes from the start of the header or of an entry.

inline constexpr std::string_view kMagic = "CSIX";
inline constexpr std::uint32_t kFormatVersion = 1;

// Feature flags. A file that sets a required flag we don't know won't open.
// Optional flags we don't know are ignored. Version 1 defines none.
inline constexpr std::uint64_t kKnownRequiredFeatures = 0;

inline constexpr std::uint8_t kHashSha1 = 1;
inline constexpr std::size_t kHashFieldBytes = 32;  // room for SHA-256 later
inline constexpr std::size_t kSha1Bytes = 20;

// Header fields.
inline constexpr std::size_t kMagicAt = 0;               // 4 bytes
inline constexpr std::size_t kFormatVersionAt = 4;       // u32
inline constexpr std::size_t kRequiredFeaturesAt = 8;    // u64
inline constexpr std::size_t kOptionalFeaturesAt = 16;   // u64
inline constexpr std::size_t kHashAlgoAt = 24;           // u8
inline constexpr std::size_t kHashLengthAt = 25;         // u8, then 6 zero bytes
inline constexpr std::size_t kCommitAt = 32;             // 32 bytes
inline constexpr std::size_t kPolicyVersionAt = 64;      // u32, then 4 zero bytes
inline constexpr std::size_t kConfigFingerprintAt = 72;  // 32 bytes: a u64, then zeros
inline constexpr std::size_t kDocCountAt = 104;          // u32
inline constexpr std::size_t kTrigramCountAt = 108;      // u32
inline constexpr std::size_t kSectionCountAt = 112;      // u32, then 4 zero bytes
inline constexpr std::size_t kSectionTableAt = 120;

// Sections, in file order. Each has an entry in the section table.
inline constexpr std::size_t kTrigramSection = 0;
inline constexpr std::size_t kPostingsSection = 1;
inline constexpr std::size_t kDocSection = 2;
inline constexpr std::size_t kPathSection = 3;
inline constexpr std::size_t kContentSection = 4;
inline constexpr std::size_t kSectionCount = 5;

// A section table entry: offset u64, length u64, codec u32, 4 zero bytes.
inline constexpr std::size_t kSectionEntryBytes = 24;
inline constexpr std::size_t kHeaderBytes = kSectionTableAt + (kSectionCount * kSectionEntryBytes);

// A trigram table entry, sorted by trigram:
//   trigram u32, doc count u32, postings offset u64 (from the start of the
//   postings section), byte length u32, 4 zero bytes.
inline constexpr std::size_t kTrigramEntryBytes = 24;

// A doc table entry; doc ID N is entry N:
//   0  path offset u64 (from the start of the paths section)
//   8  path length u32
//   12 stored content length u32
//   16 content offset u64 (from the start of the contents section)
//   24 decoded content length u32
//   28 content codec u8, flags u8, language u16
//   32 blob ID, 32 bytes (zeros for now)
inline constexpr std::size_t kDocEntryBytes = 64;

// The only content codec in version 1: the bytes as they are.
inline constexpr std::uint8_t kContentRaw = 0;

struct Section {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t codec = 0;
};

// Everything in the header, as plain values.
struct ShardHeader {
    std::uint32_t formatVersion = kFormatVersion;
    std::uint64_t requiredFeatures = 0;
    std::uint64_t optionalFeatures = 0;
    std::string commitHex;  // 40 lowercase hex characters
    std::uint32_t policyVersion = 0;
    std::uint64_t configFingerprint = 0;
    std::uint32_t docCount = 0;
    std::uint32_t trigramCount = 0;
    std::array<Section, kSectionCount> sections{};
};

// The header as kHeaderBytes bytes.
std::string encodeHeader(const ShardHeader& header);

// Reads the header from the start of a whole index file and checks it against
// the file's size: magic, version, features, hash, and that every section is
// inside the file, aligned, in order, and the right size for the counts.
// Throws IndexFileError if anything is wrong.
ShardHeader decodeHeader(std::string_view file);

}  // namespace codesearch
