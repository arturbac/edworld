# edworld: the d3d11.dll proxy, two builds of one source (README "Two builds"): edworld and edworld_eht.
# Built on Linux with clang-cl against MSVC's and the Windows SDK's headers and libraries
# (edplugin_common/cmake/clang-cl-msvc-sdk.cmake, MSVC_ROOT); the shaders compiled by the SDK's fxc.exe under wine.
cmake_minimum_required(VERSION 3.29)

# the shared library edplugin_common, a clone in this directory on the commit edplugin_common.commit pins: the
# toolchain is in it, so it is looked for before project()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/edplugin_common/CMakeLists.txt")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/edplugin_common.commit" edplugin_common_pinned LIMIT_COUNT 1)
  string(STRIP "${edplugin_common_pinned}" edplugin_common_pinned)
  message(FATAL_ERROR "no clone of edplugin_common in ${CMAKE_CURRENT_SOURCE_DIR}; in that directory:\n"
                      "  git clone https://github.com/arturbac/edplugin_common.git edplugin_common\n"
                      "  git -C edplugin_common checkout ${edplugin_common_pinned}")
endif()

project(edworld LANGUAGES CXX ASM_MASM)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  message(FATAL_ERROR "edworld builds with clang-cl: configure with a preset or -DCMAKE_TOOLCHAIN_FILE=edplugin_common/cmake/clang-cl-msvc-sdk.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# edplugin_common: the code and the build steps the plugins share, with CPM and stralgo (string operations: merge in
# place of chained +; small_vectors under it), pinned there. CPM_SOURCE_CACHE (a filled cache) and
# FETCHCONTENT_FULLY_DISCONNECTED=ON configure without the network
add_subdirectory(edplugin_common)

# simple_enum (an enumeration's value in the log by its name), the version of elite_help_tool's
cpmaddpackage(
  NAME
  simple_enum
  GITHUB_REPOSITORY
  arturbac/simple_enum
  VERSION
  0.9.5
  # tag v0.9.5, pinned to its commit - a tag can be moved
  GIT_TAG
  e4d7eae8df7f8d27993b446ce4bb51e3d2a857dd
  EXCLUDE_FROM_ALL
  YES)
if(NOT TARGET simple_enum::simple_enum)
  add_library(simple_enum::simple_enum ALIAS simple_enum)
endif()

set(gen "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${gen}")

# exports: the Windows d3d11 names from a list, thunked by EDVR's generator (tools/gen_exports.py, MIT)
add_custom_command(
  OUTPUT "${gen}/edvr_d3d11.def" "${gen}/edvr_thunks_d3d11.asm" "${gen}/edvr_exports_d3d11.inc"
  COMMAND "${Python3_EXECUTABLE}" tools/gen_exports.py --self-test
  COMMAND "${Python3_EXECUTABLE}" tools/gen_exports_from_list.py --source tools/d3d11_exports.txt --tag d3d11
          --out "${gen}" --wrap D3D11CreateDevice --wrap D3D11CreateDeviceAndSwapChain
  DEPENDS tools/gen_exports.py tools/gen_exports_from_list.py tools/d3d11_exports.txt
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "d3d11 exports"
  VERBATIM)

# Dear ImGui's D3D11 backend shaders, compiled here so the game needs no d3dcompiler_XX.dll (third_party/imgui/README.md)
edplugin_common_fxc("${gen}/imgui_vs.h" vs_4_0 vs_main g_imgui_vs "${CMAKE_SOURCE_DIR}/shaders/imgui.hlsl" /O3)
edplugin_common_fxc("${gen}/imgui_ps.h" ps_4_0 ps_main g_imgui_ps "${CMAKE_SOURCE_DIR}/shaders/imgui.hlsl" /O3)
# the list's quad under the jump panel (SM5: it reads the game's structured record buffer)
edplugin_common_fxc("${gen}/list_vs.h" vs_5_0 vs_main g_list_vs "${CMAKE_SOURCE_DIR}/shaders/list.hlsl" /O3)
edplugin_common_fxc("${gen}/list_ps.h" ps_5_0 ps_main g_list_ps "${CMAKE_SOURCE_DIR}/shaders/list.hlsl" /O3)
# the layer of clouds over a planet with an atmosphere (PoC)
edplugin_common_fxc("${gen}/clouds_vs.h" vs_5_0 vs_main g_clouds_vs "${CMAKE_SOURCE_DIR}/shaders/clouds.hlsl" /O3)
edplugin_common_fxc("${gen}/clouds_ps.h" ps_5_0 ps_main g_clouds_ps "${CMAKE_SOURCE_DIR}/shaders/clouds.hlsl" /O3)
edplugin_common_fxc("${gen}/clouds3d_vs.h" vs_5_0 vs_main g_clouds3d_vs "${CMAKE_SOURCE_DIR}/shaders/clouds3d.hlsl" /O3)
edplugin_common_fxc("${gen}/clouds3d_ps.h" ps_5_0 ps_main g_clouds3d_ps "${CMAKE_SOURCE_DIR}/shaders/clouds3d.hlsl" /O3)
edplugin_common_fxc("${gen}/clouds_up_vs.h" vs_5_0 vs_main g_clouds_up_vs "${CMAKE_SOURCE_DIR}/shaders/clouds_up.hlsl" /O3)
edplugin_common_fxc("${gen}/clouds_up_ps.h" ps_5_0 ps_main g_clouds_up_ps "${CMAKE_SOURCE_DIR}/shaders/clouds_up.hlsl" /O3)

add_custom_command(
  OUTPUT "${gen}/list_font.h"
  COMMAND "${Python3_EXECUTABLE}" tools/gen_font.py "${gen}/list_font.h"
  DEPENDS tools/gen_font.py third_party/fonts/NotoSansMono-Regular.ttf
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "list font"
  VERBATIM)

add_custom_target(edworld_generated DEPENDS
  "${gen}/edvr_d3d11.def" "${gen}/edvr_thunks_d3d11.asm" "${gen}/edvr_exports_d3d11.inc"
  "${gen}/imgui_vs.h" "${gen}/imgui_ps.h" "${gen}/list_vs.h" "${gen}/list_ps.h" "${gen}/list_font.h"
  "${gen}/clouds_vs.h" "${gen}/clouds_ps.h" "${gen}/clouds3d_vs.h" "${gen}/clouds3d_ps.h"
  "${gen}/clouds_up_vs.h" "${gen}/clouds_up_ps.h")

set(imgui "${CMAKE_SOURCE_DIR}/third_party/imgui")
add_library(edworld_imgui OBJECT
  ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp
  ${imgui}/backends/imgui_impl_dx11.cpp)
target_compile_definitions(edworld_imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                           "IMGUI_USER_CONFIG=\"imgui_config.h\"")
target_include_directories(edworld_imgui PUBLIC src ${imgui} "${gen}")
target_compile_options(edworld_imgui PRIVATE /W3)
add_dependencies(edworld_imgui edworld_generated)

add_library(edworld_thunks OBJECT "${gen}/edvr_thunks_d3d11.asm")
set_source_files_properties("${gen}/edvr_thunks_d3d11.asm" PROPERTIES GENERATED TRUE)
target_compile_options(edworld_thunks PRIVATE /nologo)
add_dependencies(edworld_thunks edworld_generated)

set(edworld_sources src/proxy.cc src/hooks.cc src/settings_log.cc src/game_state.cc src/panel_patch.cc
                    src/shader_catalog.cc src/diagnostics.cc)

foreach(variant edworld edworld_eht)
  add_library(${variant} SHARED ${edworld_sources} "${gen}/edvr_d3d11.def")
  target_link_libraries(${variant} PRIVATE edworld_imgui edworld_thunks edplugin_common::edplugin_common simple_enum::simple_enum
                        kernel32 user32 advapi32 dxgi dxguid winhttp shell32 ole32 version)
  target_compile_definitions(${variant} PRIVATE $<$<STREQUAL:${variant},edworld_eht>:EDWORLD_EHT>)
  target_compile_options(${variant} PRIVATE /W4)
  target_link_options(${variant} PRIVATE /DEBUG:FULL $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
  set_target_properties(${variant} PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${variant}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${variant}"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${variant}")
  add_dependencies(${variant} edworld_generated)
endforeach()
edplugin_common_version(EDWORLD_VERSION edworld_version.h edworld edworld_eht)
edplugin_common_pin(edworld edworld_eht)

option(EDWORLD_TESTS "build the smoke test's programs (tools/test.sh runs them under wine)" ON)
if(EDWORLD_TESTS)
  add_subdirectory(test)
endif()
