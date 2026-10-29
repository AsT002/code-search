#include "codesearch/index/shard_format.hpp"

#include <stdexcept>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/util/endian.hpp"

namespace codesearch {

namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

[[noreturn]] void damaged(const std::string& problem) {
    throw IndexFileError("index file is damaged: " + problem);
}

void appendZeros(std::string& out, std::size_t count) {
    out.append(count, '\0');
}

int hexValue(char c) {
    return c >= 'a' ? c - 'a' + 10 : c - '0';
}

// "1f..." (40 characters) to 20 bytes.
std::string hexToBytes(std::string_view hex) {
    if (hex.size() != kSha1Bytes * 2) {
        throw std::invalid_argument("commit ID must be 40 hex characters");
    }
    std::string bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>((hexValue(hex[i]) * 16) + hexValue(hex[i + 1])));
    }
    return bytes;
}

std::string bytesToHex(std::string_view bytes) {
    std::string hex;
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        hex.push_back(kHexDigits[byte / 16]);
        hex.push_back(kHexDigits[byte % 16]);
    }
    return hex;
}

bool codecAllowed(std::size_t section, std::uint32_t codec) {
    if (section == kPostingsSection) {
        return codec == static_cast<std::uint32_t>(PostingCodec::Raw) ||
               codec == static_cast<std::uint32_t>(PostingCodec::DeltaVarint);
    }
    return codec == 0;  // other sections have no codecs yet
}

// Each section must start on an 8-byte boundary, after the header and the
// section before it, and end inside the file.
void checkSections(const ShardHeader& header, std::uint64_t fileSize) {
    std::uint64_t previousEnd = kHeaderBytes;
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        const Section& section = header.sections.at(i);
        const std::string name = "section " + std::to_string(i);
        if (!codecAllowed(i, section.codec)) {
            damaged(name + " has unknown codec " + std::to_string(section.codec));
        }
        if (section.offset % 8 != 0) {
            damaged(name + " is not aligned");
        }
        if (section.offset < previousEnd) {
            damaged(name + " overlaps the part before it");
        }
        // Written as two checks so offset + length can't overflow.
        if (section.offset > fileSize || section.length > fileSize - section.offset) {
            damaged(name + " goes past the end of the file");
        }
        previousEnd = section.offset + section.length;
    }

    if (header.sections[kTrigramSection].length !=
        std::uint64_t{header.trigramCount} * kTrigramEntryBytes) {
        damaged("the trigram table doesn't match the trigram count");
    }
    if (header.sections[kDocSection].length != std::uint64_t{header.docCount} * kDocEntryBytes) {
        damaged("the doc table doesn't match the file count");
    }
}

}  // namespace

std::string encodeHeader(const ShardHeader& header) {
    std::string out;
    out.reserve(kHeaderBytes);
    out += kMagic;
    appendU32(out, header.formatVersion);
    appendU64(out, header.requiredFeatures);
    appendU64(out, header.optionalFeatures);
    appendU8(out, kHashSha1);
    appendU8(out, static_cast<std::uint8_t>(kSha1Bytes));
    appendZeros(out, 6);
    const std::string commit = hexToBytes(header.commitHex);
    out += commit;
    appendZeros(out, kHashFieldBytes - commit.size());
    appendU32(out, header.policyVersion);
    appendZeros(out, 4);
    appendU64(out, header.configFingerprint);
    appendZeros(out, 24);
    appendU32(out, header.docCount);
    appendU32(out, header.trigramCount);
    appendU32(out, static_cast<std::uint32_t>(kSectionCount));
    appendZeros(out, 4);
    for (const Section& section : header.sections) {
        appendU64(out, section.offset);
        appendU64(out, section.length);
        appendU32(out, section.codec);
        appendZeros(out, 4);
    }
    return out;
}

ShardHeader decodeHeader(std::string_view file) {
    if (file.size() < kHeaderBytes) {
        throw IndexFileError("not an index file: too small");
    }
    if (file.substr(kMagicAt, kMagic.size()) != kMagic) {
        throw IndexFileError("not an index file: wrong magic bytes");
    }

    ShardHeader header;
    header.formatVersion = readU32(file, kFormatVersionAt);
    if (header.formatVersion != kFormatVersion) {
        throw IndexFileError("index file has format version " +
                             std::to_string(header.formatVersion) + ", but this program reads " +
                             std::to_string(kFormatVersion));
    }
    header.requiredFeatures = readU64(file, kRequiredFeaturesAt);
    if ((header.requiredFeatures & ~kKnownRequiredFeatures) != 0) {
        throw IndexFileError("index file needs features this program doesn't have");
    }
    header.optionalFeatures = readU64(file, kOptionalFeaturesAt);

    if (readU8(file, kHashAlgoAt) != kHashSha1 || readU8(file, kHashLengthAt) != kSha1Bytes) {
        throw IndexFileError("index file uses a commit hash this program doesn't support");
    }
    header.commitHex = bytesToHex(file.substr(kCommitAt, kSha1Bytes));
    header.policyVersion = readU32(file, kPolicyVersionAt);
    header.configFingerprint = readU64(file, kConfigFingerprintAt);
    header.docCount = readU32(file, kDocCountAt);
    header.trigramCount = readU32(file, kTrigramCountAt);

    if (readU32(file, kSectionCountAt) != kSectionCount) {
        damaged("wrong number of sections");
    }
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        const std::size_t at = kSectionTableAt + (i * kSectionEntryBytes);
        header.sections.at(i) = Section{
            .offset = readU64(file, at),
            .length = readU64(file, at + 8),
            .codec = readU32(file, at + 16),
        };
    }
    checkSections(header, file.size());
    return header;
}

}  // namespace codesearch
