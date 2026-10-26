include(FetchContent)

# Pinned by version and hash. Bump both together.

if(CODESEARCH_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(CODESEARCH_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
    URL_HASH SHA256=b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(benchmark)
endif()
