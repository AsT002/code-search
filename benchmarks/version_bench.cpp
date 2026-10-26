#include <benchmark/benchmark.h>

#include "codesearch/version.hpp"

// Placeholder so the benchmark target builds. Replaced by real benchmarks in M1.
static void BM_Version(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(codesearch::version());
    }
}
BENCHMARK(BM_Version);
