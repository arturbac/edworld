# Cross build for 64-bit Windows on Linux: clang-cl, lld-link, llvm-lib, llvm-ml64, llvm-rc and llvm-mt from LLVM;
# the headers and libraries of MSVC and the Windows SDK from one directory in clang's /winsysroot layout
# (VC/Tools/MSVC/<version>, Windows Kits/10), e.g. the one msvc-wine installs. No MSVC compiler is run.
# That directory: MSVC_ROOT, a cache variable or else the environment variable of the same name.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT MSVC_ROOT AND DEFINED ENV{MSVC_ROOT})
  set(MSVC_ROOT "$ENV{MSVC_ROOT}")
endif()
if(NOT MSVC_ROOT OR NOT IS_DIRECTORY "${MSVC_ROOT}/VC/Tools/MSVC" OR NOT IS_DIRECTORY "${MSVC_ROOT}/Windows Kits/10")
  message(FATAL_ERROR "MSVC_ROOT='${MSVC_ROOT}': set it (cache or environment) to a directory with VC/Tools/MSVC and Windows Kits/10")
endif()
set(MSVC_ROOT "${MSVC_ROOT}" CACHE PATH "MSVC and the Windows SDK in clang's /winsysroot layout")
# try_compile projects (compiler checks) see it too
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSVC_ROOT)

find_program(CMAKE_C_COMPILER clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER clang-cl REQUIRED)
find_program(CMAKE_LINKER lld-link REQUIRED)
find_program(CMAKE_AR llvm-lib REQUIRED)
find_program(CMAKE_RC_COMPILER llvm-rc REQUIRED)
find_program(CMAKE_MT llvm-mt REQUIRED)
find_program(CMAKE_ASM_MASM_COMPILER llvm-ml64 REQUIRED)

set(CMAKE_C_FLAGS_INIT "/winsysroot${MSVC_ROOT}")
set(CMAKE_CXX_FLAGS_INIT "/winsysroot${MSVC_ROOT}")
foreach(kind EXE SHARED MODULE)
  set(CMAKE_${kind}_LINKER_FLAGS_INIT "/winsysroot:${MSVC_ROOT}")
endforeach()

set(CMAKE_FIND_ROOT_PATH "${MSVC_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
