include(FetchContent)

# Pinned by version and hash. Bump both together.

# JSON for the registry file. Header-only. This archive holds just the
# headers and CMake files, not the library's tests and docs.
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
FetchContent_MakeAvailable(json)

# RE2 for regex search (M8), and Abseil, which RE2 is built on. RE2 runs in
# linear time, so no pattern can hang a search. Only what RE2 needs is built.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(absl
  URL https://github.com/abseil/abseil-cpp/releases/download/20250814.2/abseil-cpp-20250814.2.tar.gz
  URL_HASH SHA256=f9148fb00ec98a2396bdf875c99a78e6a70afa662b107862d92b285d857a8320
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RE2_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(re2
  URL https://github.com/google/re2/releases/download/2025-11-05/re2-2025-11-05.tar.gz
  URL_HASH SHA256=87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
FetchContent_MakeAvailable(absl re2)

# cpp-httplib for the HTTP server (M9). Header-only. Plain HTTP on
# localhost, so none of its TLS or compression options are needed.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.64.0.tar.gz
  URL_HASH SHA256=1ada413782af5e67e80452c39b7c90beb6fa4fc8341e18c0896bf95684b9a620
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
FetchContent_MakeAvailable(httplib)

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

# LZ4, only for the storage experiment (M2.5). We compile its one C file
# ourselves instead of using its own CMake setup, which builds much more.
if(CODESEARCH_BUILD_BENCHMARKS)
  FetchContent_Declare(lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake  # no CMakeLists.txt here, so nothing gets added
  )
  FetchContent_MakeAvailable(lz4)
  enable_language(C)
  add_library(lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
  target_include_directories(lz4 SYSTEM PUBLIC ${lz4_SOURCE_DIR}/lib)
endif()
