# Homebrew installs LLVM as keg-only, so look there too.
set(_llvm_hints /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)

find_program(CLANG_FORMAT_EXE clang-format HINTS ${_llvm_hints})
find_program(CLANG_TIDY_EXE clang-tidy HINTS ${_llvm_hints})

# Our own sources only, so third-party code is never formatted or linted.
file(GLOB_RECURSE _codesearch_sources CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/include/*.hpp
  ${PROJECT_SOURCE_DIR}/src/*.cpp
  ${PROJECT_SOURCE_DIR}/cli/*.cpp
  ${PROJECT_SOURCE_DIR}/tests/*.cpp
  ${PROJECT_SOURCE_DIR}/benchmarks/*.cpp
)

if(CLANG_FORMAT_EXE)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${_codesearch_sources}
    COMMENT "Formatting sources"
  )
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${_codesearch_sources}
    COMMENT "Checking formatting"
  )
endif()

function(codesearch_set_clang_tidy target)
  if(NOT CODESEARCH_CLANG_TIDY)
    return()
  endif()
  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "CODESEARCH_CLANG_TIDY is ON but clang-tidy was not found")
  endif()
  set(_tidy ${CLANG_TIDY_EXE})
  if(APPLE)
    # Homebrew clang-tidy needs to be told where the macOS SDK headers are.
    # CMake 4 no longer sets CMAKE_OSX_SYSROOT by default, so ask xcrun.
    set(_sysroot "${CMAKE_OSX_SYSROOT}")
    if(NOT _sysroot)
      execute_process(COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    list(APPEND _tidy --extra-arg=-isysroot --extra-arg=${_sysroot})
  endif()
  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${_tidy}")
endfunction()
