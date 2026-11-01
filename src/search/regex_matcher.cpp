#include "codesearch/search/regex_matcher.hpp"

#include <re2/re2.h>

#include <algorithm>
#include <stdexcept>

#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

// The options every regex uses. RE2 would print errors itself without
// log_errors(false); we report them in the query error instead.
RE2::Options optionsFor(bool caseSensitive) {
    RE2::Options options;
    options.set_case_sensitive(caseSensitive);
    options.set_log_errors(false);
    return options;
}

// "(?m)" makes ^ and $ match at line starts and ends. The pattern goes in a
// group of its own, so a '|' in it can't reach past the prefix.
std::string withLineAnchors(std::string_view pattern) {
    return "(?m)(?:" + std::string(pattern) + ")";
}

}  // namespace

RegexMatcher::RegexMatcher(std::string_view pattern, bool caseSensitive) {
    if (const std::optional<std::string> problem = problemWith(pattern)) {
        throw std::invalid_argument(problem.value());
    }
    re_ = std::make_shared<const RE2>(withLineAnchors(pattern), optionsFor(caseSensitive));
}

std::optional<std::string> RegexMatcher::problemWith(std::string_view pattern) {
    // Checked on its own: inside the group, a pattern like "a)(b" would
    // become valid and mean something else.
    const RE2 re(pattern, optionsFor(true));
    if (re.ok()) {
        return std::nullopt;
    }
    return re.error();
}

bool RegexMatcher::foundIn(std::string_view text) const {
    return RE2::PartialMatch(text, *re_);
}

void RegexMatcher::findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const {
    std::size_t from = 0;
    while (from <= text.size()) {
        std::string_view found;
        // Match looks for the regex in text[from, end). It still sees the
        // text before `from`, so ^ and \b work as they would at that spot.
        if (!re_->Match(text, from, text.size(), RE2::UNANCHORED, &found, 1)) {
            return;
        }
        const auto begin = static_cast<std::size_t>(found.data() - text.data());
        const std::size_t end = begin + found.size();
        if (end > begin) {
            if (!onMatch(Span{begin, end})) {
                return;
            }
            from = end;  // the next match starts after this one
        } else {
            // An empty match isn't shown. Look again from the next character,
            // so the search moves on; RE2 only starts matches at characters.
            if (begin == text.size()) {
                return;
            }
            from = begin + std::max<std::size_t>(utf8Length(text, begin), 1);
        }
    }
}

}  // namespace codesearch
