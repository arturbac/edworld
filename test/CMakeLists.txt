# The smoke test's programs (tools/test.sh runs them under wine): test_app.exe per build (the test's shaders
# compiled by fxc), and fake_next.dll, a next element that calls d3d11.dll by name.
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

add_library(fake_next SHARED fake_next.cc)
target_compile_definitions(fake_next PRIVATE WIN32_LEAN_AND_MEAN)
target_compile_options(fake_next PRIVATE /W4)
target_link_options(fake_next PRIVATE /EXPORT:D3D11CreateDevice=fake_D3D11CreateDevice)
set_target_properties(fake_next PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${test_gen}"
  ARCHIVE_OUTPUT_DIRECTORY "${test_gen}/lib")
