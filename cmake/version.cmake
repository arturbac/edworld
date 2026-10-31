# Run at build time (cmake -DSRC=<checkout> -DOUT=<header> -DNAME=<macro> -P version.cmake): writes the header
# with NAME defined to `git describe --tags --always --dirty` ("dev" outside git), rewritten only when it changes.
execute_process(COMMAND git describe --tags --always --dirty
                WORKING_DIRECTORY "${SRC}"
                OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR version STREQUAL "")
  set(version dev)
endif()
set(text "#pragma once\n#define ${NAME} \"${version}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
