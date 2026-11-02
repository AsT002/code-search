#include "codesearch/search/regex_matcher.hpp"

#include <re2/re2.h>
#include <re2/regexp.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

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

// True if the regex uses \C, which matches any one byte. never_nl doesn't
// stop \C from matching a '\n', so a regex with it is searched one line at a
// time. And its matches can start inside a UTF-8 character, so after an
// empty match the search moves on by one byte, not one character. Walks
// RE2's parse tree, with a stack instead of recursion.
bool matchesAnyByte(const RE2& re) {
    std::vector<re2::Regexp*> stack{re.Regexp()};
    while (!stack.empty()) {
        re2::Regexp* node = stack.back();
        stack.pop_back();
        if (node->op() == re2::kRegexpAnyByte) {
            return true;
        }
        for (int i = 0; i < node->nsub(); ++i) {
            stack.push_back(node->sub()[i]);
        }
    }
    return false;
}

// The work findAll allows for the searches on one line (see there).
class LineBudget {
public:
    // Charges a search from `from` the rest of its line. False if that's
    // more than the line has left.
    bool charge(std::string_view text, std::size_t from) {
        if (!known_ || from > end_) {  // a new line
            end_ = std::min(text.find('\n', from), text.size());
            const std::size_t rest = end_ - from;
            left_ =
                rest <= kShortLine ? std::numeric_limits<std::size_t>::max() : kWorkPerByte * rest;
            known_ = true;
        }
        const std::size_t cost = end_ - from + 1;
        if (cost > left_) {
            return false;
        }
        left_ -= cost;
        return true;
    }

    // Where the line of the last search charged ends.
    std::size_t end() const { return end_; }

private:
    std::size_t end_ = 0;
    std::size_t left_ = 0;
    bool known_ = false;
};

}  // namespace

RegexMatcher::RegexMatcher(std::string_view pattern, bool caseSensitive) {
    if (const std::optional<std::string> problem = problemWith(pattern)) {
        throw std::invalid_argument(problem.value());
    }
    re_ = std::make_shared<const RE2>(withLineAnchors(pattern), optionsFor(caseSensitive));
    if (!re_->ok()) {
        throw std::invalid_argument(re_->error());
    }
    anyByte_ = matchesAnyByte(*re_);
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
    if (!RE2::PartialMatch(text, *re_)) {
        return false;
    }
    if (!anyByte_) {
        return true;
    }
    // The match found may span lines, so look in each line on its own.
    // Match still sees the text around the line, so ^, $ and \b work.
    for (std::size_t from = 0; from <= text.size(); ++from) {
        const std::size_t lineEnd = std::min(text.find('\n', from), text.size());
        if (re_->Match(text, from, lineEnd, RE2::UNANCHORED, nullptr, 0)) {
            return true;
        }
        from = lineEnd;
    }
    return false;
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
    // matches aren't highlighted. A regex with \C could run past its line,
    // so each of its searches ends at the line's end instead.
    LineBudget line;
    std::size_t from = 0;
    while (from <= text.size()) {
        const bool allowed = line.charge(text, from);
        const std::size_t lineEnd = line.end();
        if (!allowed) {
            from = lineEnd + 1;  // give up on this line, go on with the next
            continue;
        }

        std::string_view found;
        // Match looks for the regex in text[from, end). It still sees the
        // text around that, so ^, $ and \b work as they would there.
        const std::size_t end = anyByte_ ? lineEnd : text.size();
        if (!re_->Match(text, from, end, RE2::UNANCHORED, &found, 1)) {
            if (!anyByte_) {
                return;  // no match in the rest of the text
            }
            from = lineEnd + 1;  // none in this line, try the next
            continue;
        }
        const auto begin = static_cast<std::size_t>(found.data() - text.data());
        if (!found.empty()) {
            if (!onMatch(Span{begin, begin + found.size()})) {
                return;
            }
            from = begin + found.size();  // the next match starts after this one
        } else {
            // An empty match isn't shown. Look again from the next character,
            // so the search moves on; RE2 only starts matches at characters,
            // except with \C.
            if (begin == text.size()) {
                return;
            }
            from = begin + (anyByte_ ? 1 : std::max<std::size_t>(utf8Length(text, begin), 1));
        }
    }
}

}  // namespace codesearch
