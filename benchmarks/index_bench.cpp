// Records build time, search time and peak memory for the in-memory index.
// The numbers are tracked over time; nothing fails on them.

#include <benchmark/benchmark.h>
#include <sys/resource.h>

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "codesearch/index/memory_index.hpp"
#include "codesearch/search/searcher.hpp"

namespace {

using codesearch::IndexBuilder;
using codesearch::MemoryIndex;

struct File {
    std::string path;
    std::string content;
};

// Fake source files made of identifier-like words, the same on every run.
std::vector<File> makeCorpus(std::size_t fileCount, std::size_t bytesPerFile) {
    constexpr std::array kWords = {
        "int",     "return", "const",  "auto",   "void",    "if",     "else",   "for",
        "while",   "class",  "struct", "public", "private", "static", "user",   "request",
        "handler", "config", "parse",  "token",  "buffer",  "index",  "search", "result",
        "value",   "error",  "status", "count",  "size",    "update", "delete", "create",
    };
    constexpr std::array kSeparators = {" ", " ", " ", "(", ")", ";\n", ", ", ".", "_", "\n    "};

    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> word(0, kWords.size() - 1);
    std::uniform_int_distribution<std::size_t> sep(0, kSeparators.size() - 1);

    std::vector<File> files;
    files.reserve(fileCount);
    for (std::size_t i = 0; i < fileCount; ++i) {
        File file{.path = "src/file" + std::to_string(i) + ".cpp", .content = {}};
        while (file.content.size() < bytesPerFile) {
            file.content += kWords.at(word(rng));
            file.content += kSeparators.at(sep(rng));
        }
        files.push_back(std::move(file));
    }
    // A rare word in a handful of files, so "rare" queries have something to find.
    for (std::size_t i = 0; i < files.size(); i += 997) {
        files[i].content += "authenticateUser();\n";
    }
    return files;
}

const std::vector<File>& corpus() {
    static const std::vector<File> files = makeCorpus(5000, 4096);  // about 20 MB
    return files;
}

MemoryIndex buildIndex() {
    IndexBuilder builder;
    for (const File& file : corpus()) {
        builder.add(file.path, file.content);
    }
    return builder.build();
}

const MemoryIndex& sharedIndex() {
    static const MemoryIndex index = buildIndex();
    return index;
}

double peakMemoryMiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    const double bytes = static_cast<double>(usage.ru_maxrss);  // bytes on macOS
#else
    const double bytes = static_cast<double>(usage.ru_maxrss) * 1024.0;  // KiB on Linux
#endif
    return bytes / (1024.0 * 1024.0);
}

std::int64_t corpusBytes() {
    std::int64_t total = 0;
    for (const File& file : corpus()) {
        total += static_cast<std::int64_t>(file.content.size());
    }
    return total;
}

void BM_BuildIndex(benchmark::State& state) {
    corpus();  // make the corpus outside the timed loop
    for (auto _ : state) {
        MemoryIndex index = buildIndex();
        benchmark::DoNotOptimize(index);
    }
    state.SetBytesProcessed(state.iterations() * corpusBytes());
    state.counters["peak_MiB"] = peakMemoryMiB();
}
BENCHMARK(BM_BuildIndex)->Unit(benchmark::kMillisecond);

void BM_Search(benchmark::State& state, const char* text) {
    const MemoryIndex& index = sharedIndex();
    const codesearch::QueryNode query = codesearch::literal(text);
    std::size_t files = 0;
    for (auto _ : state) {
        auto results = codesearch::search(index, query);
        files = results.files.size();
        benchmark::DoNotOptimize(results);
    }
    state.counters["files"] = static_cast<double>(files);
    state.counters["peak_MiB"] = peakMemoryMiB();
}
BENCHMARK_CAPTURE(BM_Search, rare, "authenticateUser")->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Search, common, "request")->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Search, missing, "zzzzzz")->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Search, short, "if")->Unit(benchmark::kMicrosecond);

}  // namespace
