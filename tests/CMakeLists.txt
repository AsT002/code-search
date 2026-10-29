include(GoogleTest)

add_executable(codesearch_tests
  cli/cli_test.cpp
  differential/literal_search_test.cpp
  fuzz/shard_reader_fuzz_test.cpp
  integration/index_commit_test.cpp
  support/brute_force.cpp
  support/fake_git.cpp
  support/temp_dir.cpp
  support/temp_repo.cpp
  unit/candidates_test.cpp
  unit/endian_test.cpp
  unit/file_filter_test.cpp
  unit/git_errors_test.cpp
  unit/git_test.cpp
  unit/matcher_test.cpp
  unit/mmap_file_test.cpp
  unit/object_id_test.cpp
  unit/planner_test.cpp
  unit/posting_cursor_test.cpp
  unit/process_test.cpp
  unit/searcher_test.cpp
  unit/shard_test.cpp
  unit/trigram_test.cpp
  unit/utf8_test.cpp
  unit/varint_test.cpp
  unit/version_test.cpp
)
target_link_libraries(codesearch_tests PRIVATE codesearch_core GTest::gtest_main)
target_compile_definitions(codesearch_tests PRIVATE
  EXPECTED_VERSION="${PROJECT_VERSION}"
  CODE_SEARCH_BIN="$<TARGET_FILE:code-search>"  # the CLI tests run the real program
)
add_dependencies(codesearch_tests code-search)  # build the program before the tests
codesearch_set_warnings(codesearch_tests)
codesearch_set_clang_tidy(codesearch_tests)

gtest_discover_tests(codesearch_tests)
