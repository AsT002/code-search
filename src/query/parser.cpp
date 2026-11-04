#include "codesearch/query/parser.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codesearch/query/language.hpp"
#include "codesearch/search/regex_matcher.hpp"

namespace codesearch {

namespace {

// Parentheses and '-' can nest. Each level is a function call while parsing,
// so a limit keeps a silly query from using up the stack.
constexpr int kMaxDepth = 100;

enum class TokenKind {
    Word,   // text, or a filter like path:x
    Open,   // (
    Close,  // )
    Or,     // or, OR
    Not,    // a '-' in front of something
    Regex,  // /pattern/, or path:/pattern/ and file:/pattern/
    End,    // after the last token
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;  // for Word: the text with quotes and escapes removed
    // For Word: how many bytes at the start of `text` were typed plainly, not
    // in quotes or after a backslash. A filter name must be typed plainly, so
    // "path:x" in quotes is just text.
    std::size_t plainPrefix = 0;
    // For Word: nothing in it was quoted or escaped. Only a plain "or" is
    // the keyword, so or"" and o\r are text, even though their text is "or".
    bool plain = true;
    // For Regex: the pattern is for the path (path:/x/ or file:/x/).
    bool forPath = false;
    // For Regex: the token as typed, like path:/a\/b/, for error messages.
    std::string typed;
};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Reads a quoted part, starting just after the opening '"', and adds its
// text to `out`. Inside quotes, \" and \\ stand for " and \. Any other
// backslash stays as it is.
void readQuoted(std::string_view input, std::size_t& pos, std::string& out) {
    while (true) {
        if (pos == input.size()) {
            throw QueryError("a quote is missing its closing \"");
        }
        const char c = input[pos];
        if (c == '"') {
            ++pos;
            return;
        }
        if (c == '\\' && pos + 1 < input.size() &&
            (input[pos + 1] == '"' || input[pos + 1] == '\\')) {
            out.push_back(input[pos + 1]);
            pos += 2;
            continue;
        }
        out.push_back(c);
        ++pos;
    }
}

// Reads one word, like a shell does: it ends at a space, and quoted parts
// join the text around them. A '(' inside a word is plain text, and so is a
// ')' that closes it, so "printf(" and "f(x)" are words. Any other ')' ends
// the word and closes a group.
Token readWord(std::string_view input, std::size_t& pos) {
    Token token{.kind = TokenKind::Word, .text = "", .plainPrefix = 0, .plain = true};
    bool allPlain = true;  // nothing quoted or escaped so far
    int openParens = 0;
    while (pos < input.size()) {
        const char c = input[pos];
        if (isSpace(c) || (c == ')' && openParens == 0)) {
            break;
        }
        if (c == '"') {
            allPlain = false;
            ++pos;
            readQuoted(input, pos, token.text);
            continue;
        }
        if (c == '\\') {
            if (pos + 1 == input.size()) {
                throw QueryError("a \\ at the end of the query has nothing to make plain");
            }
            allPlain = false;
            token.text.push_back(input[pos + 1]);
            pos += 2;
            continue;
        }
        if (c == '(') {
            ++openParens;
        } else if (c == ')') {
            --openParens;
        }
        token.text.push_back(c);
        ++pos;
        if (allPlain) {
            token.plainPrefix = token.text.size();
        }
    }
    token.plain = allPlain;
    return token;
}

// Where a regex can end: each '/' that isn't escaped (it has an even number
// of backslashes right before it) and is followed by a space, a ')' or the
// end. Found once for the whole query, so reading a regex never scans the
// rest of the query again: a query of many words like /usr/bin stays fast.
std::vector<std::size_t> regexEnds(std::string_view input) {
    std::vector<std::size_t> ends;
    std::size_t backslashes = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '/' && backslashes % 2 == 0) {
            const bool closes =
                i + 1 == input.size() || isSpace(input[i + 1]) || input[i + 1] == ')';
            if (closes) {
                ends.push_back(i);
            }
        }
        backslashes = c == '\\' ? backslashes + 1 : 0;
    }
    return ends;
}

// Reads a regex starting at input[pos], its opening '/'. The pattern runs to
// the first place after it where a regex can end (`ends`, from regexEnds).
// Backslashes stay as they are, for RE2, except that \/ stands for '/'.
// Returns nothing, and leaves `pos` alone, if there's no such place or the
// pattern would be empty: then it's plain text. So "/usr/bin" and "// TODO"
// search for text, and "// /foo/" is the text // and the regex foo.
std::optional<std::string> readRegex(std::string_view input, std::size_t& pos,
                                     const std::vector<std::size_t>& ends) {
    // lower_bound: the first place after the opening '/', by binary search.
    const auto end = std::lower_bound(ends.begin(), ends.end(), pos + 1);
    if (end == ends.end() || *end == pos + 1) {
        return std::nullopt;
    }
    std::string pattern;
    for (std::size_t at = pos + 1; at < *end; ++at) {
        if (input[at] == '\\' && at + 1 < *end) {
            if (input[at + 1] == '/') {
                pattern += '/';
            } else {
                pattern += input.substr(at, 2);
            }
            ++at;
            continue;
        }
        pattern += input[at];
    }
    pos = *end + 1;
    return pattern;
}

// A regex token at input[pos]: /pattern/ on its own, or as the value of
// path: or file:. Moves `pos` past it. Returns nothing, and leaves `pos`
// alone, if there isn't one (see readRegex).
std::optional<Token> regexToken(std::string_view input, std::size_t& pos,
                                const std::vector<std::size_t>& ends) {
    const std::string_view rest = input.substr(pos);
    const bool forPath = rest.starts_with("path:/") || rest.starts_with("file:/");
    std::size_t start = forPath ? pos + 5 : pos;
    if (input[start] != '/') {
        return std::nullopt;
    }
    std::optional<std::string> pattern = readRegex(input, start, ends);
    if (!pattern.has_value()) {
        return std::nullopt;
    }
    Token token{
        .kind = TokenKind::Regex,
        .text = std::move(pattern.value()),
        .forPath = forPath,
        .typed = std::string(input.substr(pos, start - pos)),
    };
    pos = start;
    return token;
}

std::vector<Token> tokenize(std::string_view input) {
    const std::vector<std::size_t> ends = regexEnds(input);
    std::vector<Token> tokens;
    std::size_t pos = 0;
    while (true) {
        while (pos < input.size() && isSpace(input[pos])) {
            ++pos;
        }
        if (pos == input.size()) {
            break;
        }
        const char c = input[pos];
        const bool afterNot = !tokens.empty() && tokens.back().kind == TokenKind::Not;
        if (c == '(') {
            tokens.push_back({.kind = TokenKind::Open});
            ++pos;
            continue;
        }
        if (c == ')') {
            tokens.push_back({.kind = TokenKind::Close});
            ++pos;
            continue;
        }
        // '-' excludes what comes right after it. A '-' on its own, or
        // followed by another '-' (like --verbose), is plain text.
        if (c == '-' && pos + 1 < input.size()) {
            const char after = input[pos + 1];
            if (!isSpace(after) && after != ')' && after != '-') {
                tokens.push_back({.kind = TokenKind::Not});
                ++pos;
                continue;
            }
        }
        if (std::optional<Token> pattern = regexToken(input, pos, ends)) {
            tokens.push_back(std::move(pattern.value()));
            continue;
        }
        Token word = readWord(input, pos);
        if (word.plain && !afterNot && (word.text == "or" || word.text == "OR")) {
            word.kind = TokenKind::Or;
        }
        tokens.push_back(std::move(word));
    }
    tokens.push_back({.kind = TokenKind::End});
    return tokens;
}

// Characters that do something in a regex, and so would be odd in text.
bool isRegexSyntax(char c) {
    return std::string_view("[]()*+?|{}^$").find(c) != std::string_view::npos;
}

// A warning if `text`, typed plainly as one word, looks like a regex that
// doesn't start the word: like 4./[x]*/, which is searched as plain text.
// A path like src/server/ has no regex syntax between its slashes, so it
// gets none.
std::optional<std::string> regexInWordWarning(const std::string& text) {
    const std::size_t slash = text.find('/');
    if (slash == 0 || slash == std::string::npos || text.size() < slash + 3 || text.back() != '/') {
        return std::nullopt;
    }
    const std::string inner = text.substr(slash + 1, text.size() - slash - 2);
    bool looksLikeRegex = false;
    for (const char c : inner) {
        if (isRegexSyntax(c)) {
            looksLikeRegex = true;
        }
    }
    if (!looksLikeRegex) {
        return std::nullopt;
    }
    // The fix: the text before the slash, escaped, joined to the regex.
    std::string fixed = "/";
    for (const char c : text.substr(0, slash)) {
        if (isRegexSyntax(c) || c == '.' || c == '/') {
            fixed += '\\';
        }
        fixed += c;
    }
    fixed += inner + "/";
    return text +
           " is searched as plain text, because a regex has to start the term. "
           "For a regex, write " +
           fixed;
}

// A filter in a word, like "path" and "src/" for path:src/.
struct Filter {
    std::string name;
    std::string value;
};

// The filter in `token`, or nothing if it's plain text. A filter's name must
// be one we know, typed plainly, so "path:x" in quotes is text. Throws
// QueryError if the value is missing, like in "path:".
std::optional<Filter> filterIn(const Token& token) {
    if (token.kind != TokenKind::Word) {
        return std::nullopt;
    }
    const std::size_t colon = token.text.find(':');
    if (colon == std::string::npos || colon >= token.plainPrefix) {
        return std::nullopt;
    }
    Filter filter{.name = token.text.substr(0, colon), .value = token.text.substr(colon + 1)};
    if (filter.name != "repo" && filter.name != "path" && filter.name != "file" &&
        filter.name != "lang" && filter.name != "case") {
        return std::nullopt;
    }
    if (filter.value.empty()) {
        throw QueryError(filter.name + ": needs a value, like " + filter.name + ":x");
    }
    return filter;
}

// A recursive descent parser: one function per level of the grammar, from
// the loosest (or) to the tightest (a single word).
//
//   or-expr  = and-expr { "or" and-expr }
//   and-expr = unary { unary }
//   unary    = "-" unary | "(" or-expr ")" | word
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    ParsedQuery parse() {
        QueryNode root = parseOr();
        if (peek().kind == TokenKind::Close) {
            throw QueryError("there's a ) without a matching (");
        }
        return ParsedQuery{
            .root = std::move(root),
            .caseSensitive = caseSensitive_,
            .warnings = std::move(warnings_),
        };
    }

private:
    const Token& peek() const { return tokens_[next_]; }

    // Moves past the current token. Never moves past End.
    const Token& take() {
        const Token& token = tokens_[next_];
        if (token.kind != TokenKind::End) {
            ++next_;
        }
        return token;
    }

    void enter() {
        if (++depth_ > kMaxDepth) {
            throw QueryError("the query is nested too deeply (more than " +
                             std::to_string(kMaxDepth) + " levels)");
        }
    }

    void leave() { --depth_; }

    QueryNode parseOr() {
        std::vector<QueryNode> branches;
        branches.push_back(parseAnd());
        while (peek().kind == TokenKind::Or) {
            take();
            branches.push_back(parseAnd());
        }
        if (branches.size() == 1) {
            return std::move(branches[0]);
        }
        return anyOf(std::move(branches));
    }

    QueryNode parseAnd() {
        std::vector<QueryNode> items;
        bool sawCase = false;
        while (peek().kind != TokenKind::End && peek().kind != TokenKind::Close &&
               peek().kind != TokenKind::Or) {
            // case: sets an option for the whole query instead of matching.
            const std::optional<Filter> filter = filterIn(peek());
            if (filter.has_value() && filter->name == "case") {
                take();
                setCase(filter->value);
                sawCase = true;
                continue;
            }
            items.push_back(parseUnary());
        }
        if (items.empty()) {
            throw QueryError(std::string(emptyMessage(sawCase)));
        }
        if (items.size() == 1) {
            return std::move(items[0]);
        }
        return allOf(std::move(items));
    }

    // Why parseAnd found nothing.
    std::string_view emptyMessage(bool sawCase) const {
        if (sawCase) {
            return "case: needs something to search for next to it";
        }
        const bool afterOr = next_ > 0 && tokens_[next_ - 1].kind == TokenKind::Or;
        if (afterOr || peek().kind == TokenKind::Or) {
            return "\"or\" needs something on both sides. To search for the word, use \"or\" "
                   "in quotes";
        }
        if (peek().kind == TokenKind::Close) {
            return "there's nothing inside ( )";
        }
        const bool afterOpen = next_ > 0 && tokens_[next_ - 1].kind == TokenKind::Open;
        if (afterOpen) {
            return "a ( is missing its )";  // like "foo (": the query ends after it
        }
        return "the query is empty";
    }

    QueryNode parseUnary() {
        // The tokenizer only makes a Not when a word or '(' follows it, so
        // there's always something to exclude.
        const Token& token = take();
        if (token.kind == TokenKind::Not) {
            enter();
            QueryNode child = parseUnary();
            leave();
            return negate(std::move(child));
        }
        if (token.kind == TokenKind::Open) {
            enter();
            QueryNode inner = parseOr();
            if (peek().kind != TokenKind::Close) {
                throw QueryError("a ( is missing its )");
            }
            take();
            leave();
            return inner;
        }
        if (token.kind == TokenKind::Regex) {
            return parseRegex(token);
        }
        return parseWord(token);
    }

    static QueryNode parseRegex(const Token& token) {
        // Only to check it, with the options and memory RegexMatcher
        // gives it; the evaluator compiles it again with the query's options.
        if (const std::optional<std::string> problem = RegexMatcher::problemWith(token.text)) {
            throw QueryError("the regex " + token.typed + " isn't valid: " + problem.value());
        }
        return token.forPath ? pathRegex(token.text) : regex(token.text);
    }

    QueryNode parseWord(const Token& token) {
        const std::optional<Filter> filter = filterIn(token);
        if (filter.has_value()) {
            return parseFilter(filter.value());
        }
        if (token.text.empty()) {
            throw QueryError("\"\" is empty. Leave it out, or put text inside the quotes");
        }
        // Only for words typed plainly: quotes or a backslash say it's text.
        if (token.plain) {
            if (std::optional<std::string> warning = regexInWordWarning(token.text)) {
                warnings_.push_back(std::move(warning.value()));
            }
        }
        return literal(token.text);
    }

    static QueryNode parseFilter(const Filter& filter) {
        if (filter.name == "repo") {
            return repoFilter(filter.value);
        }
        if (filter.name == "path" || filter.name == "file") {
            return pathFilter(filter.value);
        }
        if (filter.name == "lang") {
            std::optional<std::string> name = languageName(filter.value);
            if (!name.has_value()) {
                throw QueryError("unknown language \"" + filter.value +
                                 "\". Known: " + knownLanguages());
            }
            return langFilter(std::move(name.value()));
        }
        // parseAnd takes every other case:, so this one has a '-' in front.
        throw QueryError("case: can't have a - in front of it");
    }

    void setCase(const std::string& value) {
        if (value != "yes" && value != "no") {
            throw QueryError("case: takes yes or no");
        }
        const bool wanted = value == "yes";
        if (caseSensitive_.has_value() && caseSensitive_.value() != wanted) {
            throw QueryError("the query has both case:yes and case:no");
        }
        caseSensitive_ = wanted;
    }

    std::vector<Token> tokens_;
    std::size_t next_ = 0;  // the token we're looking at
    int depth_ = 0;
    std::optional<bool> caseSensitive_;
    std::vector<std::string> warnings_;
};

}  // namespace

ParsedQuery parseQuery(std::string_view text) {
    return Parser(tokenize(text)).parse();
}

}  // namespace codesearch
