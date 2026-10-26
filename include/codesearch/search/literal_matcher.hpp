#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace codesearch {

// A byte range [begin, end) inside a file's content.
struct Span {
    std::size_t begin = 0;
    std::size_t end = 0;

    // Compares begin first, then end. Gives us ==, <, > and so on for free.
    friend auto operator<=>(const Span&, const Span&) = default;
};

// Finds a fixed string in text. Matches are reported left to right and don't
// overlap: "aa" in "aaaa" matches at 0 and 2. An empty pattern matches nothing.
class LiteralMatcher {
public:
    LiteralMatcher(std::string_view pattern, bool caseSensitive);

    // Calls onMatch for each match. Return false from onMatch to stop.
    void findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const;

    // True if the pattern occurs at least once.
    bool foundIn(std::string_view text) const;

    bool emptyPattern() const { return pattern_.empty(); }

private:
    // Position of the first match at or after `from`, or npos if none.
    std::size_t find(std::string_view text, std::size_t from) const;

    std::string pattern_;  // already lowercased when !caseSensitive_
    bool caseSensitive_;
};

}  // namespace codesearch
