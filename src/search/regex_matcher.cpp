#include "codesearch/search/regex_matcher.hpp"

#include <re2/re2.h>
#include <re2/regexp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <vector>

#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

// The options every regex uses. RE2 would print errors itself without
// log_errors(false); we report them in the query error instead.
// never_nl: a match never takes in a '\n', so it stays within a line, as
// with grep (except with \C, see matchesAnyByte).
// max_mem: what RE2 may use for one regex, mostly for its DFA, the fast
// way it matches. When that runs out it falls back to a way that costs text
// size times pattern size: for /(a|b).{1000}(a|b){1000}c/ on 1 MiB, 17 s
// with the default 8 MiB and 0.06 s with 64 MiB. It's a cap, not an
// allocation. Some patterns, like /a.{1000}c/, need more states than any
// cap allows and stay slow on long lines.
constexpr std::int64_t kRegexMemory = std::int64_t{64} * 1024 * 1024;

RE2::Options optionsFor(bool caseSensitive) {
    RE2::Options options;
    options.set_case_sensitive(caseSensitive);
    options.set_never_nl(true);
    options.set_log_errors(false);
    options.set_max_mem(kRegexMemory);
    return options;
}

// "(?m)" makes ^ and $ match at line starts and ends. A flag at the start
// covers the whole pattern, '|' and all, so the pattern needs no group
// around it (which a \Q without \E at its end would swallow).
std::string withLineAnchors(std::string_view pattern) {
    return "(?m)" + std::string(pattern);
}

// How long findAll may look for every match in one text before it looks
// for only one match per line: kHighlightTime, plus kHighlightTimePerByte
// for each byte of it. Finding even very dense matches takes far less per
// byte; time that grows with the square of a line's length takes far more.
constexpr std::chrono::nanoseconds kHighlightTime = std::chrono::milliseconds{100};
constexpr std::chrono::nanoseconds kHighlightTimePerByte = std::chrono::microseconds{1};

// The CPU time this thread has used so far. Unlike the time on a wall
// clock, it doesn't grow while the process is paused or waits for a CPU.
std::chrono::nanoseconds threadCpuTime() {
    timespec now{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec};
}

// findAll's time limit for a text of `bytes` bytes. It counts the CPU time
// the search uses, so a busy machine or a paused process doesn't use it
// up. Reading that clock costs about 6 times as much as the wall clock,
// so the wall clock is read first: CPU time never runs ahead of it, so
// until the wall clock passes the limit, the CPU time can't have either.
class HighlightClock {
public:
    explicit HighlightClock(std::size_t bytes)
        : limit_(kHighlightTime + (kHighlightTimePerByte * bytes)),
          wallStart_(std::chrono::steady_clock::now()),
          cpuStart_(threadCpuTime()) {}

    // Called before each search. The clock is read every time, though that
    // costs about half as much as a short search: one search can take
    // a second, so skipping even a few reads could run far past the limit.
    bool timeIsUp() const {
        if (std::chrono::steady_clock::now() - wallStart_ <= limit_) {
            return false;
        }
        return threadCpuTime() - cpuStart_ > limit_;
    }

private:
    std::chrono::nanoseconds limit_;
    std::chrono::steady_clock::time_point wallStart_;
    std::chrono::nanoseconds cpuStart_;
};

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

// Where a match can start next after `at`: the next character, since RE2
// only starts matches at characters, or with \C (`anyByte`) the next byte.
std::size_t nextStart(std::string_view text, std::size_t at, bool anyByte) {
    if (anyByte) {
        return at + 1;
    }
    return at + std::max<std::size_t>(utf8Length(text, at), 1);
}

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

RegexMatcher RegexMatcher::withOwnCopy() const {
    RegexMatcher copy = *this;
    // The same pattern and options, so it compiles the same way.
    copy.re_ = std::make_shared<const RE2>(re_->pattern(), re_->options());
    return copy;
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
    // the line to see if a*z matches, before taking the a, so the time
    // grows with the square of the line's length. RE2 can't say how much
    // work a search did, so after a time limit findAll looks for only the
    // first match in each line that's left: each line is then searched
    // once, so the time is linear again. Lines with a match are still
    // found; only their later matches aren't highlighted. Ordinary files
    // take a tiny part of the limit.
    //
    // A regex with \C could run past its line, and searches after the limit
    // stop at the line's end too, so those searches end there.
    const HighlightClock clock(text.size());
    bool firstPerLine = false;  // the time is up: one match per line
    std::size_t lineEnd = 0;    // where the line `from` is on ends, if lineKnown
    bool lineKnown = false;
    std::size_t from = 0;
    while (from <= text.size()) {
        if (!firstPerLine && clock.timeIsUp()) {
            firstPerLine = true;
        }
        const bool byLine = anyByte_ || firstPerLine;
        if (byLine && (!lineKnown || from > lineEnd)) {
            lineEnd = std::min(text.find('\n', from), text.size());
            lineKnown = true;
        }

        std::string_view found;
        // Match looks for the regex in text[from, end). It still sees the
        // text around that, so ^, $ and \b work as they would there.
        const std::size_t end = byLine ? lineEnd : text.size();
        if (!re_->Match(text, from, end, RE2::UNANCHORED, &found, 1)) {
            if (!byLine) {
                return;  // no match in the rest of the text
            }
            from = lineEnd + 1;  // none in this line, try the next
            continue;
        }
        const auto begin = static_cast<std::size_t>(found.data() - text.data());
        // An empty match isn't shown.
        if (!found.empty() && !onMatch(Span{begin, begin + found.size()})) {
            return;
        }
        if (firstPerLine) {
            // This line is done, even after an empty match: looking again
            // from the next character could read the rest of the line each
            // time, as for /a*z|/ on a line of a's. So only a line where the
            // regex matches nothing first and something later is missed,
            // and only in a text slow enough to reach the limit.
            from = lineEnd + 1;
        } else if (!found.empty()) {
            from = begin + found.size();  // the next match starts after this one
        } else if (begin == text.size()) {
            return;
        } else {
            // Look again from the next place a match can start, so the
            // search moves on.
            from = nextStart(text, begin, anyByte_);
        }
    }
}

}  // namespace codesearch
