# The smoke test's programs (tools/test.sh runs them under wine): test_app.exe per build (the test's shaders
# compiled by fxc), fake_next.dll, a next element that calls d3d11.dll by name, and test_foreign.dll, an overlay
# making a device of its own from another folder.
set(test_gen "${CMAKE_BINARY_DIR}/test")
file(MAKE_DIRECTORY "${test_gen}")
set(test_shaders "")
foreach(shader panel_vs:vs_5_0 other_vs:vs_5_0 found_vs:vs_5_0 panel_ps:ps_5_0 catalog_cs:cs_5_0)
  string(REPLACE ":" ";" parts "${shader}")
  list(GET parts 0 name)
  list(GET parts 1 profile)
  edworld_fxc("${test_gen}/${name}.h" ${profile} main g_${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}.hlsl")
  list(APPEND test_shaders "${test_gen}/${name}.h")
endforeach()
add_custom_target(edworld_test_shaders DEPENDS ${test_shaders})

foreach(variant edworld edworld_eht)
  add_executable(${variant}_test_app test_app.cc)
  target_compile_definitions(${variant}_test_app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                             $<$<STREQUAL:${variant},edworld_eht>:EDWORLD_EHT>)
  target_include_directories(${variant}_test_app PRIVATE "${test_gen}")
  target_compile_options(${variant}_test_app PRIVATE /W4)
  target_link_libraries(${variant}_test_app PRIVATE d3d11 dxguid)
  set_target_properties(${variant}_test_app PROPERTIES
    OUTPUT_NAME test_app
    RUNTIME_OUTPUT_DIRECTORY "${test_gen}/${variant}")
  add_dependencies(${variant}_test_app edworld_test_shaders)
endforeach()

# the render test: the clouds' volume shader drawn with the system d3d11 for scenes of several atmospheres, its buffer
# by src/clouds_values.h, checked by numbers and against test/data/render/*.png
add_executable(render_test render_test.cc)
target_compile_definitions(render_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS EDWORLD_EHT)
target_include_directories(render_test PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/gen")
target_compile_options(render_test PRIVATE /W4)
target_link_libraries(render_test PRIVATE d3d11 dxgi dxguid windowscodecs ole32 simple_enum::simple_enum)  # runtime.h (clouds_values.h)
set_target_properties(render_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${test_gen}")
add_dependencies(render_test edworld_eht)

add_library(fake_next SHARED fake_next.cc)
target_compile_definitions(fake_next PRIVATE WIN32_LEAN_AND_MEAN)
target_compile_options(fake_next PRIVATE /W4)
target_link_options(fake_next PRIVATE /EXPORT:D3D11CreateDevice=fake_D3D11CreateDevice)
set_target_properties(fake_next PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${test_gen}"
  ARCHIVE_OUTPUT_DIRECTORY "${test_gen}/lib")

add_library(test_foreign SHARED test_foreign.cc)
target_compile_definitions(test_foreign PRIVATE WIN32_LEAN_AND_MEAN)
target_compile_options(test_foreign PRIVATE /W4)
target_link_libraries(test_foreign PRIVATE d3d11)
set_target_properties(test_foreign PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${test_gen}"
  ARCHIVE_OUTPUT_DIRECTORY "${test_gen}/lib")
