#include "codesearch/search/literal_matcher.hpp"

#include <array>
#include <cstdint>
#include <cstring>

#include "codesearch/util/ascii.hpp"

namespace codesearch {

LiteralMatcher::LiteralMatcher(std::string_view pattern, bool caseSensitive)
    : pattern_(pattern), caseSensitive_(caseSensitive) {
    if (!caseSensitive_) {
        for (char& c : pattern_) {
            c = foldAscii(c);
        }
    }
}

namespace {

// How many start positions find() checks in one step.
constexpr std::size_t kBlock = 32;

// The bit that turns 'A'-'Z' into 'a'-'z' when set.
constexpr unsigned kCaseBit = 0x20;

// One byte of the pattern, set up for a fast compare. For a lowercase letter
// when case doesn't matter, `mask` is the case bit: b | 0x20 is that letter
// only for the letter in either case, so one compare covers both. For any
// other byte, `mask` is 0 and the compare is exact.
struct ByteTest {
    unsigned value = 0;
    unsigned mask = 0;

    ByteTest(char c, bool caseSensitive) : value(static_cast<unsigned char>(c)) {
        if (!caseSensitive && c >= 'a' && c <= 'z') {
            mask = kCaseBit;
        }
    }

    bool operator()(char b) const { return (static_cast<unsigned char>(b) | mask) == value; }
};

// True if `pattern` (already lowercased) is at `text` when case is ignored.
bool sameFolded(const char* text, std::string_view pattern) {
    for (std::size_t j = 0; j < pattern.size(); ++j) {
        if (foldAscii(text[j]) != pattern[j]) {
            return false;
        }
    }
    return true;
}

// True for the bytes that are most common in code: lowercase letters,
// spaces and tabs.
bool isCommonByte(char c) {
    return (c >= 'a' && c <= 'z') || c == ' ' || c == '\t';
}

using Hits = std::array<std::uint8_t, kBlock>;

// Sets hits[j] to 1 if start position i + j passes the test of the
// pattern's first and last bytes (`lastOffset` apart), and to 0 if not.
// The loop has no branches, so the compiler turns it into vector
// instructions that test many bytes in each step.
void testBlock(std::string_view text, std::size_t i, std::size_t lastOffset, const ByteTest& first,
               const ByteTest& last, Hits& hits) {
    for (std::size_t j = 0; j < kBlock; ++j) {
        // & on numbers instead of &&: && would skip the second test, which
        // is a branch.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) j < kBlock
        hits[j] = static_cast<std::uint8_t>(static_cast<unsigned>(first(text[i + j])) &
                                            static_cast<unsigned>(last(text[i + j + lastOffset])));
    }
}

// The first j whose hit matches in full (matchesAt(i + j)), or kBlock if
// none does. Most positions have no hit, so the flags are read 8 at a time
// as one 64-bit number, and 8 empty ones are skipped with one test. memcpy
// is the safe way to read bytes as a number; it becomes one load.
template <typename MatchesAt>
std::size_t firstFullMatch(const Hits& hits, std::size_t i, const MatchesAt& matchesAt) {
    for (std::size_t group = 0; group < kBlock; group += 8) {
        std::uint64_t any = 0;
        std::memcpy(&any, hits.data() + group, sizeof(any));
        if (any == 0) {
            continue;
        }
        for (std::size_t j = group; j < group + 8; ++j) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) j < kBlock
            if (hits[j] != 0 && matchesAt(i + j)) {
                return j;
            }
        }
    }
    return kBlock;
}

}  // namespace

std::size_t LiteralMatcher::find(std::string_view text, std::size_t from) const {
    // memchr, which string_view::find uses, jumps quickly from one copy of
    // the first byte to the next. That wins when the byte is rare, so only
    // patterns starting with a common byte go to the filter below. (Measured
    // in M7, see docs/BENCHMARKS.md.) Ignoring case needs the filter: memchr
    // can't look for both cases at once.
    if (caseSensitive_ && !isCommonByte(pattern_.front())) {
        return text.find(pattern_, from);
    }
    if (pattern_.size() > text.size() || from > text.size() - pattern_.size()) {
        return std::string_view::npos;
    }
    const std::size_t lastStart = text.size() - pattern_.size();
    const std::size_t lastOffset = pattern_.size() - 1;
    const ByteTest first(pattern_.front(), caseSensitive_);
    const ByteTest last(pattern_.back(), caseSensitive_);
    const auto matchesAt = [&](std::size_t i) {
        const std::string_view middle(pattern_.data() + 1, pattern_.size() - 1);
        return caseSensitive_ ? text.compare(i + 1, middle.size(), middle) == 0
                              : sameFolded(text.data() + i + 1, middle);
    };

    // A start position can only match if the pattern's first and last bytes
    // are in the right places. Test that for kBlock positions at once, and
    // compare in full only the positions that pass.
    std::size_t i = from;
    Hits hits{};
    while (lastStart - i >= kBlock) {  // i never passes lastStart here
        testBlock(text, i, lastOffset, first, last, hits);
        const std::size_t j = firstFullMatch(hits, i, matchesAt);
        if (j < kBlock) {
            return i + j;
        }
        i += kBlock;
    }
    // The last few positions, one at a time.
    for (; i <= lastStart; ++i) {
        if (first(text[i]) && last(text[i + lastOffset]) && matchesAt(i)) {
            return i;
        }
    }
    return std::string_view::npos;
}

void LiteralMatcher::findAll(std::string_view text,
                             const std::function<bool(Span)>& onMatch) const {
    if (pattern_.empty()) {
        return;
    }
    std::size_t from = 0;
    while (true) {
        const std::size_t at = find(text, from);
        if (at == std::string_view::npos) {
            return;
        }
        const bool keepGoing = onMatch(Span{at, at + pattern_.size()});
        if (!keepGoing) {
            return;
        }
        from = at + pattern_.size();  // continue after this match, so matches don't overlap
    }
}

bool LiteralMatcher::foundIn(std::string_view text) const {
    return !pattern_.empty() && find(text, 0) != std::string_view::npos;
}

}  // namespace codesearch
