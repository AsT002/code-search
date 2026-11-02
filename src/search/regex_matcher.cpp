#include "codesearch/search/regex_matcher.hpp"

#include <re2/re2.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

// The options every regex uses. RE2 would print errors itself without
// log_errors(false); we report them in the query error instead.
// never_nl: a match never takes in a '\n', so it stays within a line, as
// with grep. That also keeps the search for each match within its line.
RE2::Options optionsFor(bool caseSensitive) {
    RE2::Options options;
    options.set_case_sensitive(caseSensitive);
    options.set_never_nl(true);
    options.set_log_errors(false);
    return options;
}

// "(?m)" makes ^ and $ match at line starts and ends. A flag at the start
// covers the whole pattern, '|' and all, so the pattern needs no group
// around it (which a \Q without \E at its end would swallow).
std::string withLineAnchors(std::string_view pattern) {
    return "(?m)" + std::string(pattern);
}

// Lines findAll searches in full, and how much work it allows per byte of
// a longer line (see there).
constexpr std::size_t kShortLine = 1024;
constexpr std::size_t kWorkPerByte = 256;

}  // namespace

RegexMatcher::RegexMatcher(std::string_view pattern, bool caseSensitive) {
    if (const std::optional<std::string> problem = problemWith(pattern)) {
        throw std::invalid_argument(problem.value());
    }
    re_ = std::make_shared<const RE2>(withLineAnchors(pattern), optionsFor(caseSensitive));
    if (!re_->ok()) {
        throw std::invalid_argument(re_->error());
    }
}

std::optional<std::string> RegexMatcher::problemWith(std::string_view pattern) {
    // Checked on its own, as typed, so the error message is about that.
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
    // Each search for a match is linear, but finding all of them isn't
    // always: for /a*z|a/ on a line of a's, every search runs to the end of
    // the line to see if a*z matches, before taking the a. A search never
    // runs past its line (never_nl), so charging each one the rest of its
    // line is a safe upper bound on its work. Lines up to kShortLine bytes,
    // which is nearly all code, are never cut short. On a longer line, once
    // the charges reach kWorkPerByte per byte of it, the rest of that line
    // isn't searched: the file still matches, but that line's remaining
    // matches aren't highlighted.
    std::size_t lineEnd = 0;  // where the line `from` is on ends
    std::size_t budget = 0;   // what's left for the rest of that line
    bool lineKnown = false;
    std::size_t from = 0;
    while (from <= text.size()) {
        if (!lineKnown || from > lineEnd) {
            lineEnd = std::min(text.find('\n', from), text.size());
            const std::size_t rest = lineEnd - from;
            budget =
                rest <= kShortLine ? std::numeric_limits<std::size_t>::max() : kWorkPerByte * rest;
            lineKnown = true;
        }
        const std::size_t cost = lineEnd - from + 1;
        if (cost > budget) {
            from = lineEnd + 1;  // give up on this line, go on with the next
            continue;
        }
        budget -= cost;

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
