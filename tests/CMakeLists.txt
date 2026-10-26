include(GoogleTest)

add_executable(codesearch_tests
  differential/literal_search_test.cpp
  support/brute_force.cpp
  unit/candidates_test.cpp
  unit/matcher_test.cpp
  unit/planner_test.cpp
  unit/searcher_test.cpp
  unit/trigram_test.cpp
  unit/version_test.cpp
)
target_link_libraries(codesearch_tests PRIVATE codesearch_core GTest::gtest_main)
target_compile_definitions(codesearch_tests PRIVATE EXPECTED_VERSION="${PROJECT_VERSION}")
codesearch_set_warnings(codesearch_tests)
codesearch_set_clang_tidy(codesearch_tests)

gtest_discover_tests(codesearch_tests)
