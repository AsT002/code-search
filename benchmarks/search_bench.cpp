// Warm search timings: opens every repo in a workspace once, then runs each
// query from a list several times in this process. See docs/BENCHMARKS.md.
//
//   search_bench <workspace> <queries file> [--rounds n] [--k n] [--threads n]
//
// The queries file has one "group<TAB>query" per line; '#' starts a comment.
// For each query it prints the median over the rounds of: time to the first
// result, to the first k results, to all results, and to finish with
// --max-results k. Then the same per group, as p50 and p95 over its queries.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codesearch/app/repo_manager.hpp"
#include "codesearch/query/parser.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/search/candidates.hpp"
#include "codesearch/search/repo_search.hpp"
#include "codesearch/util/thread_pool.hpp"

namespace {

using namespace codesearch;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct QueryLine {
    std::string group;
    std::string text;
};

std::vector<QueryLine> readQueries(const std::string& file) {
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("can't read " + file);
    }
    std::vector<QueryLine> queries;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (line.empty() || line.starts_with('#') || tab == std::string::npos) {
            continue;
        }
        queries.push_back({.group = line.substr(0, tab), .text = line.substr(tab + 1)});
    }
    return queries;
}

// One run of one query.
struct Timing {
    double first = 0;   // ms to the first result; the whole run if there's none
    double firstK = 0;  // ms to the k-th result; the whole run if there are fewer
    double all = 0;     // ms to the end
    std::size_t results = 0;
};

Timing runOnce(std::span<const SearchTarget> targets, const SearchRequest& request, std::size_t k) {
    Timing timing;
    std::optional<double> first;
    std::optional<double> firstK;
    const Clock::time_point start = Clock::now();
    searchRepos(targets, request, [&](const ResultFile& file) {
        timing.results += file.lines.empty() ? 1 : file.lines.size();
        if (!first.has_value()) {
            first = msSince(start);
        }
        if (!firstK.has_value() && timing.results >= k) {
            firstK = msSince(start);
        }
    });
    timing.all = msSince(start);
    timing.first = first.value_or(timing.all);
    timing.firstK = firstK.value_or(timing.all);
    return timing;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// The value below which `share` of the values are, by the nearest-rank method.
double percentile(std::vector<double> values, double share) {
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(share * static_cast<double>(values.size()) + 0.999);
    return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

struct QueryResult {
    std::string group;
    std::string text;
    std::size_t candidates = 0;
    std::size_t candidateBytes = 0;  // the content of the candidates, all checked
    std::size_t results = 0;
    double candidatesMs = 0;
    double first = 0;
    double firstK = 0;
    double all = 0;
    double limited = 0;  // the whole run with maxResults = k
};

QueryResult measure(std::span<const SearchTarget> targets, const QueryLine& line,
                    std::size_t rounds, std::size_t k, ThreadPool* pool) {
    const ParsedQuery parsed = parseQuery(line.text);
    SearchRequest request;
    request.query = parsed.root;
    request.options.caseSensitive = parsed.caseSensitive.value_or(false);
    request.pool = pool;

    QueryResult result{.group = line.group, .text = line.text};
    std::vector<double> candidatesMs;
    std::vector<double> first;
    std::vector<double> firstK;
    std::vector<double> all;
    std::vector<double> limited;
    for (std::size_t round = 0; round < rounds; ++round) {
        const Clock::time_point start = Clock::now();
        std::size_t candidates = 0;
        std::vector<std::vector<DocId>> found;
        for (const SearchTarget& target : targets) {
            const QueryNode query = resolveRepo(request.query, target.name);
            found.emplace_back();
            if (query != constant(false)) {
                found.back() =
                    findCandidates(*target.index, planCandidates(query, request.options));
                candidates += found.back().size();
            }
        }
        candidatesMs.push_back(msSince(start));
        result.candidates = candidates;
        result.candidateBytes = 0;
        for (std::size_t t = 0; t < targets.size(); ++t) {
            for (const DocId doc : found[t]) {
                targets[t].index->withContent(doc, [&](std::string_view content) {
                    result.candidateBytes += content.size();
                });
            }
        }

        request.maxResults = 0;
        const Timing timing = runOnce(targets, request, k);
        first.push_back(timing.first);
        firstK.push_back(timing.firstK);
        all.push_back(timing.all);
        result.results = timing.results;

        request.maxResults = k;
        limited.push_back(runOnce(targets, request, k).all);
    }
    result.candidatesMs = median(candidatesMs);
    result.first = median(first);
    result.firstK = median(firstK);
    result.all = median(all);
    result.limited = median(limited);
    return result;
}

void printRow(const std::vector<std::string>& cells) {
    std::cout << "|";
    for (const std::string& cell : cells) {
        std::cout << " " << cell << " |";
    }
    std::cout << "\n";
}

std::string ms(double value) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), value < 10 ? "%.2f" : "%.1f", value);
    return buffer.data();
}

// A whole number of at least 1, or nothing if `text` isn't one.
std::optional<std::size_t> positive(const std::string& text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

int usage(const std::string& problem) {
    std::cerr << "search_bench: " << problem
              << "\nusage: search_bench <workspace> <queries file> [--rounds n] [--k n] "
                 "[--threads n]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 2) {
        return usage("needs a workspace and a queries file");
    }
    std::size_t rounds = 5;
    std::size_t k = 10;
    std::size_t threads = 1;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        std::size_t* target = args[i] == "--rounds"    ? &rounds
                              : args[i] == "--k"       ? &k
                              : args[i] == "--threads" ? &threads
                                                       : nullptr;
        if (target == nullptr) {
            return usage("unknown option " + args[i]);
        }
        const std::optional<std::size_t> value =
            i + 1 < args.size() ? positive(args[i + 1]) : std::nullopt;
        if (!value.has_value()) {
            return usage(args[i] + " needs a whole number of at least 1");
        }
        *target = value.value();
    }

    // A wrong path would otherwise give a table of zeros that looks real.
    const Workspace workspace(args[0]);
    if (!std::filesystem::exists(workspace.registryFile())) {
        return usage("no code-search workspace at " + args[0]);
    }
    std::vector<OpenRepo> repos;
    try {
        repos = openRepos(workspace, constant(true));
    } catch (const std::exception& error) {
        return usage(error.what());
    }
    if (repos.empty()) {
        return usage("the workspace at " + args[0] + " has no repos");
    }
    std::vector<SearchTarget> targets;
    for (const OpenRepo& repo : repos) {
        targets.push_back({.name = repo.name, .index = repo.index.get()});
    }
    std::unique_ptr<ThreadPool> pool;
    if (threads > 1) {
        pool = std::make_unique<ThreadPool>(threads);
    }
    std::vector<QueryLine> queries;
    try {
        queries = readQueries(args[1]);
    } catch (const std::exception& error) {
        return usage(error.what());
    }
    if (queries.empty()) {
        return usage("no queries in " + args[1]);
    }

    std::vector<QueryResult> results;
    for (const QueryLine& line : queries) {
        // One run first, so the files are in the page cache.
        measure(targets, line, 1, k, pool.get());
        results.push_back(measure(targets, line, rounds, k, pool.get()));
    }

    const std::string kText = std::to_string(k);
    std::cout << "### Queries (ms, median of " << rounds << " rounds, " << threads << " thread"
              << (threads == 1 ? "" : "s") << ")\n\n";
    printRow({"Group", "Query", "Candidates", "Candidate MiB", "Results", "Candidates ms", "First",
              "First " + kText, "All", "Limit " + kText});
    printRow({"---", "---", "---:", "---:", "---:", "---:", "---:", "---:", "---:", "---:"});
    for (const QueryResult& r : results) {
        printRow({r.group, "`" + r.text + "`", std::to_string(r.candidates),
                  ms(static_cast<double>(r.candidateBytes) / 1048576.0), std::to_string(r.results),
                  ms(r.candidatesMs), ms(r.first), ms(r.firstK), ms(r.all), ms(r.limited)});
    }

    std::cout << "\n### Groups (ms, p50 / p95 over the group's queries)\n\n";
    printRow({"Group", "Queries", "First", "First " + kText, "All", "Limit " + kText});
    printRow({"---", "---:", "---:", "---:", "---:", "---:"});
    std::map<std::string, std::vector<const QueryResult*>> groups;
    std::vector<std::string> order;
    for (const QueryResult& r : results) {
        if (!groups.contains(r.group)) {
            order.push_back(r.group);
        }
        groups[r.group].push_back(&r);
    }
    for (const std::string& group : order) {
        const auto column = [&](double QueryResult::* field) {
            std::vector<double> values;
            for (const QueryResult* r : groups[group]) {
                values.push_back(r->*field);
            }
            return ms(percentile(values, 0.5)) + " / " + ms(percentile(values, 0.95));
        };
        printRow({group, std::to_string(groups[group].size()), column(&QueryResult::first),
                  column(&QueryResult::firstK), column(&QueryResult::all),
                  column(&QueryResult::limited)});
    }
    return 0;
}
