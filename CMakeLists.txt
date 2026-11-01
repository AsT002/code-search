cmake_minimum_required(VERSION 3.25)

project(code-search VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODESEARCH_BUILD_TESTS "Build tests" ON)
option(CODESEARCH_BUILD_BENCHMARKS "Build benchmarks" ON)
option(CODESEARCH_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CODESEARCH_TSAN "Build with ThreadSanitizer (can't be combined with CODESEARCH_SANITIZE)" OFF)
option(CODESEARCH_WERROR "Treat warnings as errors" OFF)
option(CODESEARCH_CLANG_TIDY "Run clang-tidy while compiling" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Tools)

if(CODESEARCH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

if(CODESEARCH_TSAN AND CODESEARCH_SANITIZE)
  message(FATAL_ERROR "CODESEARCH_TSAN and CODESEARCH_SANITIZE can't both be on: "
                      "ThreadSanitizer and AddressSanitizer can't be in one build")
endif()

if(CODESEARCH_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

include(Dependencies)  # each one is only fetched when something needs it

# Core library
add_library(codesearch_core
  src/app/index_commit.cpp
  src/app/repo_manager.cpp
  src/git/git_repo.cpp
  src/git/git_runner.cpp
  src/index/file_filter.cpp
  src/index/memory_index.cpp
  src/index/posting_cursor.cpp
  src/index/shard_format.cpp
  src/index/shard_reader.cpp
  src/index/shard_writer.cpp
  src/index/trigram.cpp
  src/query/language.cpp
  src/query/parser.cpp
  src/query/planner.cpp
  src/repo/github_url.cpp
  src/repo/publisher.cpp
  src/repo/registry.cpp
  src/repo/workspace.cpp
  src/search/candidates.cpp
  src/search/evaluator.cpp
  src/search/json_output.cpp
  src/search/literal_matcher.cpp
  src/search/repo_search.cpp
  src/search/searcher.cpp
  src/util/atomic_file.cpp
  src/util/file_lock.cpp
  src/util/mmap_file.cpp
  src/util/object_id.cpp
  src/util/process.cpp
  src/util/thread_pool.cpp
  src/util/utf8.cpp
  src/util/varint.cpp
  src/version.cpp
)
target_include_directories(codesearch_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(codesearch_core PUBLIC Threads::Threads PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(codesearch_core PRIVATE CODESEARCH_VERSION="${PROJECT_VERSION}")
codesearch_set_warnings(codesearch_core)
codesearch_set_clang_tidy(codesearch_core)

# CLI
add_executable(code-search cli/main.cpp)
target_link_libraries(code-search PRIVATE codesearch_core)
codesearch_set_warnings(code-search)
codesearch_set_clang_tidy(code-search)

if(CODESEARCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CODESEARCH_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
