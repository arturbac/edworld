# Dear ImGui (vendored)

Dear ImGui v1.91.9b (github.com/ocornut/imgui, tag `v1.91.9b`, commit f5befd2d29e66809cd1110a152e375a7f1981f06),
MIT, `LICENSE.txt`. The same version the EHT overlay builds with. Only the files edworld compiles are here.

One change, in `backends/imgui_impl_dx11.cpp`: the vertex and pixel shaders come from `imgui_vs.h` / `imgui_ps.h`,
compiled by `fxc` at build time from `shaders/imgui.hlsl` (the backend's own shader source), instead of `D3DCompile`
at run time; the `d3dcompiler` include and link are gone. Nothing else is changed in the backend; edworld's copy of
the pixel shader (`shaders/imgui.hlsl`) scales the output's alpha by the alpha of a texture at t1 (see there).
