// Random files and queries, checked against the brute-force reference.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "../support/brute_force.hpp"
#include "codesearch/index/memory_index.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/searcher.hpp"

namespace codesearch {
namespace {

using testing::bruteForceSearch;
using testing::TestFile;

// A small alphabet so trigrams repeat a lot and candidates often turn out to
// be false matches. Includes upper case, CR/LF, a NUL and non-ASCII bytes.
// The "sv" suffix keeps the NUL; a plain string literal would stop there.
using namespace std::string_view_literals;
constexpr std::string_view kAlphabet = "abcAB_ \n\r\0\xC3\xA9\xFF"sv;

class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}

    std::size_t number(std::size_t lo, std::size_t hi) {
        return std::uniform_int_distribution<std::size_t>(lo, hi)(rng_);
    }

    std::string text(std::size_t length) {
        std::string out;
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(kAlphabet[number(0, kAlphabet.size() - 1)]);
        }
        return out;
    }

    // Mostly pieces of real files, so many queries do match something.
    std::string queryText(const std::vector<TestFile>& files) {
        const std::size_t kind = number(0, 9);
        if (kind < 5 && !files.empty()) {
            const std::string& content = files[number(0, files.size() - 1)].content;
            if (content.empty()) {
                return {};
            }
            const std::size_t start = number(0, content.size() - 1);
            const std::size_t length = number(0, std::min<std::size_t>(8, content.size() - start));
            std::string piece = content.substr(start, length);
            if (kind == 4) {  // flip the case of a piece of real text
                for (char& c : piece) {
                    if (c >= 'a' && c <= 'z') {
                        c = static_cast<char>(c - 'a' + 'A');
                    } else if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
            }
            return piece;
        }
        return text(number(0, 6));
    }

    QueryNode query(const std::vector<TestFile>& files) {
        const std::size_t count = number(1, 3);
        if (count == 1) {
            return literal(queryText(files));
        }
        std::vector<QueryNode> children;
        children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            children.push_back(literal(queryText(files)));
        }
        return allOf(std::move(children));
    }

private:
    std::mt19937 rng_;
};

std::string describe(const QueryNode& query) {
    if (const auto* lit = std::get_if<LiteralNode>(&query.value)) {
        std::string out = "\"";
        for (const char c : lit->text) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7F) {
                out.push_back(c);
            } else {
                out += std::format("\\x{:02X}", byte);
            }
        }
        return out + "\"";
    }
    std::string out = "AND(";
    for (const QueryNode& child : std::get<AndNode>(query.value).children) {
        out += describe(child) + " ";
    }
    return out + ")";
}

// Compares the real search with the brute-force one for a single query.
void checkQuery(const MemoryIndex& index, const std::vector<TestFile>& files,
                const QueryNode& query, const SearchOptions& options) {
    const SearchResults expected = bruteForceSearch(files, query, options);

    // Every real match must survive the trigram step.
    const std::vector<DocId> candidates = findCandidates(index, planCandidates(query));
    for (const FileMatch& file : expected.files) {
        ASSERT_TRUE(std::binary_search(candidates.begin(), candidates.end(), file.doc))
            << "the index dropped " << file.path;
    }

    ASSERT_EQ(search(index, query, options), expected);
}

TEST(LiteralSearchDifferential, MatchesBruteForce) {
    constexpr std::uint32_t kSeeds = 300;
    constexpr int kQueriesPerSeed = 40;

    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Generator gen(seed);

        std::vector<TestFile> files;
        IndexBuilder builder;
        const std::size_t fileCount = gen.number(0, 12);
        for (std::size_t i = 0; i < fileCount; ++i) {
            TestFile file{.path = "f" + std::to_string(i), .content = gen.text(gen.number(0, 120))};
            builder.add(file.path, file.content);
            files.push_back(std::move(file));
        }
        const MemoryIndex index = builder.build();

        for (int q = 0; q < kQueriesPerSeed; ++q) {
            const QueryNode query = gen.query(files);
            const SearchOptions options{.caseSensitive = gen.number(0, 1) == 1};
            SCOPED_TRACE("seed " + std::to_string(seed) + ", query " + describe(query) +
                         (options.caseSensitive ? ", case-sensitive" : ""));

            checkQuery(index, files, query, options);
            if (HasFatalFailure()) {
                return;  // stop at the first failure so the output stays short
            }
        }
    }
}

}  // namespace
}  // namespace codesearch
