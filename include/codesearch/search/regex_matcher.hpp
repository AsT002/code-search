#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "codesearch/search/literal_matcher.hpp"

// RE2's class, declared here so this header doesn't need RE2's.
namespace re2 {
class RE2;
}

namespace codesearch {

// Finds an RE2 regular expression in text. RE2 runs in time linear in the
// text, so no pattern can make a search hang. The text is read as UTF-8.
// ^ and $ match at the start and end of every line, not only of the text.
// Without case sensitivity, RE2 folds case its own way, for non-ASCII
// letters too (unlike LiteralMatcher, which folds only ASCII).
//
// Cheap to copy: copies share one compiled pattern, which is safe to use
// from many threads at once.
class RegexMatcher {
public:
    // Throws std::invalid_argument if `pattern` isn't a valid regex.
    RegexMatcher(std::string_view pattern, bool caseSensitive);

    // Why `pattern` isn't a valid regex, like "missing ): (ab", or nothing
    // if it is.
    static std::optional<std::string> problemWith(std::string_view pattern);

    // True if the regex matches somewhere, even an empty match.
    bool foundIn(std::string_view text) const;

    // Calls onMatch for each non-empty match, left to right, without
    // overlaps, as RE2 finds them: each one starts as early as it can.
    // Return false from onMatch to stop.
    void findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const;

    // The compiled regex, for the planner (see planCandidates).
    const re2::RE2& compiled() const { return *re_; }

private:
    std::shared_ptr<const re2::RE2> re_;
    bool anyByte_ = false;  // see matchesAnyByte in the .cpp
};

}  // namespace codesearch
