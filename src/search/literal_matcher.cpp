#include "codesearch/search/literal_matcher.hpp"

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

std::size_t LiteralMatcher::find(std::string_view text, std::size_t from) const {
    if (caseSensitive_) {
        return text.find(pattern_, from);
    }
    if (pattern_.size() > text.size()) {
        return std::string_view::npos;
    }
    // Try each start position. Simple on purpose; M7 will measure if it's too slow.
    const std::size_t lastStart = text.size() - pattern_.size();
    for (std::size_t i = from; i <= lastStart; ++i) {
        std::size_t j = 0;
        while (j < pattern_.size() && foldAscii(text[i + j]) == pattern_[j]) {
            ++j;
        }
        if (j == pattern_.size()) {
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
