# Run at build time (cmake -DSRC=<checkout> -DOUT=<header> -DNAME=<macro> -P version.cmake): writes the header
# with NAME defined to the build's version, rewritten only when it changes. A release (HEAD on its tag vX.Y.Z, no
# uncommitted changes to tracked files) is "vX.Y.Z"; any other build is "<the last vX.Y.Z tag>-<HEAD's short hash>",
# with "-dirty" when tracked files differ from HEAD (or git status fails); "v0.0.0-<hash>" before the first tag;
# "dev" outside git, without a commit, or when git cannot run. A pre-release tag (vX.Y.Z-beta.N) counts as a release.
# The same file in edloader, edworld, edupscale, edwide_screen and edshader-catch.
function(git_out var)
  execute_process(COMMAND git ${ARGN}
                  WORKING_DIRECTORY "${SRC}"
                  OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    set(out "")
  endif()
  set(${var} "${out}" PARENT_SCOPE)
endfunction()

git_out(hash rev-parse --short=7 HEAD)
if(hash STREQUAL "")
  set(version dev)
else()
  execute_process(COMMAND git --no-optional-locks status --porcelain --untracked-files=no
                  WORKING_DIRECTORY "${SRC}"
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    set(changes "?")
  endif()
  git_out(exact describe --tags --exact-match --match "v[0-9]*" HEAD)
  git_out(tag describe --tags --abbrev=0 --match "v[0-9]*" HEAD)
  if(tag STREQUAL "")
    set(tag v0.0.0)
  endif()
  if(NOT exact STREQUAL "" AND changes STREQUAL "")
    set(version "${exact}")
  elseif(changes STREQUAL "")
    set(version "${tag}-${hash}")
  else()
    set(version "${tag}-${hash}-dirty")
  endif()
endif()
set(text "#pragma once\n#define ${NAME} \"${version}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
