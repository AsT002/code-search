// Random query trees (and, or, -, repo:, path:, lang:) over several random
// repos, checked against the brute-force reference. Also prints each tree as
// query text and checks the parser reads it back the same.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../support/brute_force.hpp"
#include "codesearch/index/memory_index.hpp"
#include "codesearch/query/parser.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/repo_search.hpp"

namespace codesearch {
namespace {

using testing::BruteFile;
using testing::bruteForceRepoSearch;
using testing::BruteLine;
using testing::TestFile;
using testing::TestRepo;

using namespace std::string_view_literals;
// A small alphabet, so text repeats a lot. Includes upper case, CR/LF, a NUL,
// the characters the query language treats specially, and non-ASCII bytes.
constexpr std::string_view kAlphabet = "abcAB_ \n\r\0\"\\(-\xC3\xA9\xFF"sv;
// std::array of string_view: fixed at compile time, so nothing can go wrong
// before main() starts, which a global std::vector<std::string> can't promise.
constexpr std::array<std::string_view, 8> kPaths = {
    "Makefile", "a.py", "docs/README", "lib/b.py", "main.go", "n.txt", "src/util.go", "x/Makefile",
};
constexpr std::array<std::string_view, 4> kRepoNames = {"r0", "r1", "r2", "r3"};  // r3 never added
constexpr std::array<std::string_view, 4> kLanguages = {"go", "python", "make", "cpp"};
// Characters RE2 folds in ways ASCII-only folding doesn't: KELVIN SIGN with
// k, LONG S with s, and é with É. Text sometimes gets one of these whole.
constexpr std::array<std::string_view, 7> kFoldingPieces = {
    "k", "K", "\xE2\x84\xAA", "s", "\xC5\xBF", "\xC3\xA9", "\xC3\x89",
};
// Pieces of regexes, each valid on its own.
constexpr std::array<std::string_view, 18> kRegexAtoms = {
    "a",    "b",   "k",   "s", "\xC3\xA9", ".",   " ",    "\\n",          "[ab]",
    "[^a]", "\\w", "\\b", "^", "$",        "\\(", "\\\\", "[k\\x{212A}]", "A",
};

class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}

    std::size_t number(std::size_t lo, std::size_t hi) {
        return std::uniform_int_distribution<std::size_t>(lo, hi)(rng_);
    }

    // A random item of a vector or an array.
    template <typename List>
    const auto& pick(const List& items) {
        return items.at(number(0, items.size() - 1));
    }

    std::string text(std::size_t length) {
        std::string out;
        for (std::size_t i = 0; i < length; ++i) {
            if (number(0, 9) == 0) {
                out += pick(kFoldingPieces);
            } else {
                out.push_back(kAlphabet[number(0, kAlphabet.size() - 1)]);
            }
        }
        return out;
    }

    // A random regex: atoms joined, grouped, repeated and put in
    // alternations, some with empty branches. Always valid.
    std::string pattern(int depth) {
        std::string out;
        const std::size_t parts = number(1, 3);
        for (std::size_t p = 0; p < parts; ++p) {
            std::string part = depth < 2 && number(0, 3) == 0 ? "(" + pattern(depth + 1) + ")"
                                                              : std::string(pick(kRegexAtoms));
            switch (number(0, 7)) {
                case 0:
                    part += '*';
                    break;
                case 1:
                    part += '+';
                    break;
                case 2:
                    part += '?';
                    break;
                case 3:
                    part += "{0,2}";
                    break;
                default:
                    break;
            }
            out += part;
        }
        if (number(0, 4) == 0) {
            out +=
                number(0, 1) == 0 ? "|" : "|" + pattern(depth + 1);  // an empty branch, or another
        }
        return out;
    }

    // A nonempty piece of `from`, or random text if that's empty.
    std::string piece(const std::string& from) {
        if (from.empty() || number(0, 3) == 0) {
            return text(number(1, 4));
        }
        const std::size_t start = number(0, from.size() - 1);
        return from.substr(start, number(1, std::min<std::size_t>(5, from.size() - start)));
    }

    std::vector<TestRepo> repos() {
        std::vector<TestRepo> out;
        const std::size_t count = number(1, 3);
        for (std::size_t r = 0; r < count; ++r) {
            TestRepo repo{.name = std::string(kRepoNames.at(r)), .files = {}};
            for (const std::string_view path : kPaths) {
                if (number(0, 1) == 0) {
                    repo.files.push_back(
                        {.path = std::string(path), .content = text(number(0, 60))});
                }
            }
            out.push_back(std::move(repo));
        }
        return out;
    }

    QueryNode query(const std::vector<TestRepo>& repos, int depth) {
        const std::size_t kind = number(0, depth >= 3 ? 3 : 7);
        switch (kind) {
            case 0:
            case 1: {
                const TestRepo& repo = pick(repos);
                return literal(piece(repo.files.empty() ? "" : pick(repo.files).content));
            }
            case 2:
                return number(0, 1) == 0 ? pathFilter(piece(std::string(pick(kPaths))))
                                         : langFilter(std::string(pick(kLanguages)));
            case 3:
                return repoFilter(std::string(pick(kRepoNames)));
            case 4:
                return number(0, 1) == 0 ? negate(query(repos, depth + 1)) : regex(pattern(0));
            default: {
                std::vector<QueryNode> children;
                const std::size_t count = number(2, 3);
                children.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    children.push_back(query(repos, depth + 1));
                }
                return kind == 5 ? anyOf(std::move(children)) : allOf(std::move(children));
            }
        }
    }

private:
    std::mt19937 rng_;
};

// `text` in quotes, with " and \ escaped.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

// The tree as query text, with every group in parentheses.
std::string toText(const QueryNode& query) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        return quoted(lit->text);
    }
    if (const auto* path = std::get_if<PathNode>(&query.value)) {
        return "path:" + quoted(path->text);
    }
    if (const auto* pattern = std::get_if<RegexNode>(&query.value)) {
        std::string out = "/";
        for (const char c : pattern->pattern) {
            out += c == '/' ? "\\/" : std::string(1, c);
        }
        return out + "/";
    }
    if (const auto* lang = std::get_if<LangNode>(&query.value)) {
        return "lang:" + lang->name;
    }
    if (const auto* repo = std::get_if<RepoNode>(&query.value)) {
        return "repo:" + quoted(repo->name);
    }
    if (const auto* notNode = std::get_if<NotNode>(&query.value)) {
        return "-(" + toText(notNode->child[0]) + ")";
    }
    const auto* orNode = std::get_if<OrNode>(&query.value);
    const std::vector<QueryNode>& children =
        orNode != nullptr ? orNode->children : std::get<AndNode>(query.value).children;
    std::string out;
    for (const QueryNode& child : children) {
        if (!out.empty()) {
            out += orNode != nullptr ? " or " : " ";
        }
        out += "(" + toText(child) + ")";
    }
    return out;
}

std::vector<BruteFile> realSearch(const std::vector<TestRepo>& repos,
                                  const std::vector<MemoryIndex>& indexes,
                                  const SearchRequest& request, SearchStatus& status) {
    std::vector<SearchTarget> targets;
    targets.reserve(repos.size());
    for (std::size_t i = 0; i < repos.size(); ++i) {
        targets.push_back({.name = repos[i].name, .index = &indexes[i]});
    }
    std::vector<BruteFile> found;
    status = searchRepos(targets, request, [&](const ResultFile& file) {
        BruteFile out{.repo = file.repo, .path = file.path, .lines = {}};
        for (const ResultLine& line : file.lines) {
            out.lines.push_back(BruteLine{.number = line.number, .matches = line.matches});
        }
        found.push_back(std::move(out));
    });
    return found;
}

// The first `limit` results of `all`, cut the way searchRepos cuts them.
// Sets `cut` if anything was left out.
std::vector<BruteFile> firstResults(const std::vector<BruteFile>& all, std::size_t limit,
                                    bool& cut) {
    std::vector<BruteFile> kept;
    cut = false;
    for (const BruteFile& file : all) {
        if (limit == 0) {
            cut = true;
            break;
        }
        BruteFile copy = file;
        if (copy.lines.empty()) {
            --limit;
        } else if (copy.lines.size() > limit) {
            copy.lines.resize(limit);
            cut = true;
            kept.push_back(std::move(copy));
            break;
        } else {
            limit -= copy.lines.size();
        }
        kept.push_back(std::move(copy));
    }
    return kept;
}

TEST(QuerySearchDifferential, MatchesBruteForce) {
    constexpr std::uint32_t kSeeds = 200;
    constexpr int kQueriesPerSeed = 40;
    ThreadPool threads(4);
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Generator gen(seed);
        const std::vector<TestRepo> repos = gen.repos();
        std::vector<MemoryIndex> indexes;
        for (const TestRepo& repo : repos) {
            IndexBuilder builder;
            for (const TestFile& file : repo.files) {
                builder.add(file.path, file.content);
            }
            indexes.push_back(builder.build());
        }

        for (int q = 0; q < kQueriesPerSeed; ++q) {
            const QueryNode query = gen.query(repos, 0);
            const std::string text = toText(query);
            SCOPED_TRACE("seed " + std::to_string(seed) + ", query: " + text);
            ASSERT_EQ(parseQuery(text).root, query) << "the parser read it back differently";

            const SearchOptions options{.caseSensitive = gen.number(0, 3) == 0};
            const std::size_t limit = gen.number(1, 6);
            const std::vector<BruteFile> expected = bruteForceRepoSearch(repos, query, options);

            // Every real match must survive the trigram step in its repo.
            for (std::size_t r = 0; r < repos.size(); ++r) {
                const std::vector<DocId> candidates = findCandidates(
                    indexes[r], planCandidates(resolveRepo(query, repos[r].name), options));
                for (const BruteFile& file : expected) {
                    if (file.repo != repos[r].name) {
                        continue;
                    }
                    const auto at = std::find_if(repos[r].files.begin(), repos[r].files.end(),
                                                 [&](const TestFile& f) {
                                                     return f.path == file.path;
                                                 });
                    const auto doc = static_cast<DocId>(at - repos[r].files.begin());
                    ASSERT_TRUE(std::binary_search(candidates.begin(), candidates.end(), doc))
                        << "the index dropped " << file.repo << ":" << file.path;
                }
            }

            // On this thread, then on the pool: the same results either way.
            for (ThreadPool* pool : {static_cast<ThreadPool*>(nullptr), &threads}) {
                SCOPED_TRACE(pool == nullptr ? "one thread" : "thread pool");
                SearchRequest request;
                request.query = query;
                request.options = options;
                request.pool = pool;
                SearchStatus status = SearchStatus::Cancelled;
                ASSERT_EQ(realSearch(repos, indexes, request, status), expected);
                ASSERT_EQ(status, SearchStatus::Complete);

                // With a limit: the same results, cut at the same place.
                request.maxResults = limit;
                bool cut = false;
                const std::vector<BruteFile> limited = firstResults(expected, limit, cut);
                ASSERT_EQ(realSearch(repos, indexes, request, status), limited)
                    << "limit " << limit;
                ASSERT_EQ(status, cut ? SearchStatus::Truncated : SearchStatus::Complete)
                    << "limit " << limit;
            }
        }
    }
}

}  // namespace
}  // namespace codesearch
