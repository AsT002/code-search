function(codesearch_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
  )
  # libc++ headers include other headers they don't need to, so a file can
  # use std::optional without including <optional> and still compile here,
  # then fail with another standard library. This turns that off for our own
  # code, so a missing include fails the build. Other libraries ignore it.
  target_compile_definitions(${target} PRIVATE _LIBCPP_REMOVE_TRANSITIVE_INCLUDES)
  if(CODESEARCH_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
