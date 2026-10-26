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
  if(CODESEARCH_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
