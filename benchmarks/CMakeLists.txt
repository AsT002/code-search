add_executable(codesearch_benchmarks
  index_bench.cpp
)
target_link_libraries(codesearch_benchmarks PRIVATE codesearch_core benchmark::benchmark_main)
codesearch_set_warnings(codesearch_benchmarks)

# Measures index size and speed choices on a real repo. See docs/BENCHMARKS.md.
add_executable(storage_experiment
  storage_experiment.cpp
)
target_link_libraries(storage_experiment PRIVATE codesearch_core lz4)
codesearch_set_warnings(storage_experiment)
