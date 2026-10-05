#!/usr/bin/env bash
# Builds edworld's d3d11.dll on Linux through msvc-wine (cl, ml64, link).
# Environment: MSVC_WINE_ENV (msvc-wine's env.sh), FXC (fxc.exe of a Windows SDK), RELEASE_DLL (a released EDVR d3d11.dll).
# Exports: the Windows d3d11 list read from a released EDVR d3d11.dll (RELEASE_DLL), thunked by EDVR's generator.
# Two builds of one source: edworld (on its own) and edworld_eht (EDWORLD_EHT: works with EHT); README "Two builds".
# Usage: tools/build.sh   (output: build/edworld/edworld.dll + .pdb, build/edworld_eht/edworld_eht.dll + .pdb)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
need() { [[ -n ${!1:-} ]] || { echo "$0: set $1 ($2)" >&2; exit 1; }; }
need MSVC_WINE_ENV "msvc-wine's env.sh"
need FXC "fxc.exe of a Windows SDK"
need RELEASE_DLL "a released EDVR d3d11.dll: its export list"
source "$MSVC_WINE_ENV"
cd "$HERE"
VER="$(git describe --tags --always --dirty 2>/dev/null || echo dev)"
mkdir -p build/gen
OBJ=$(mktemp -d build/obj.XXXXXX)   # fresh per run, nothing deleted
python3 tools/gen_exports.py --self-test >/dev/null
python3 tools/gen_exports_from_release.py --source "$RELEASE_DLL" --tag d3d11 --out build/gen \
    --wrap D3D11CreateDevice --wrap D3D11CreateDeviceAndSwapChain
# Dear ImGui's D3D11 backend shaders, compiled here so the game needs no d3dcompiler_XX.dll (third_party/imgui/README.md)
WINEDEBUG=-all wine "$FXC" /nologo /O3 /T vs_4_0 /E vs_main /Vn g_imgui_vs /Fh build/gen/imgui_vs.h shaders/imgui.hlsl >/dev/null
WINEDEBUG=-all wine "$FXC" /nologo /O3 /T ps_4_0 /E ps_main /Vn g_imgui_ps /Fh build/gen/imgui_ps.h shaders/imgui.hlsl >/dev/null
python3 tools/gen_font.py build/gen/list_font.h
ml64 /nologo /c /Fo$OBJ/thunks.obj build/gen/edvr_thunks_d3d11.asm
IMGUI=third_party/imgui
IMGUI_FLAGS=("/DIMGUI_USER_CONFIG=\"imgui_config.h\"" /I"src" /I"$IMGUI" /I"build/gen")
cl /nologo /c /O2 /MT /std:c++latest /EHsc /W3 /Z7 /MP8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS \
    "${IMGUI_FLAGS[@]}" /Fo$OBJ/ $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
    $IMGUI/backends/imgui_impl_dx11.cpp
for V in edworld edworld_eht; do
  DEFS=()
  [[ $V == edworld_eht ]] && DEFS=(/DEDWORLD_EHT)
  mkdir -p "$OBJ/$V" "build/$V"
  cl /nologo /c /O2 /MT /std:c++latest /EHsc /W4 /Z7 /MP8 \
      /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE "${DEFS[@]}" \
      "/DEDWORLD_VERSION=\"$VER\"" "${IMGUI_FLAGS[@]}" /Fo$OBJ/$V/ src/proxy.cc src/hooks.cc src/settings_log.cc src/game_state.cc src/panel_patch.cc
  link /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO /DEBUG:FULL /OPT:REF /OPT:ICF \
      /PDB:build/$V/$V.pdb /DEF:build/gen/edvr_d3d11.def /OUT:build/$V/$V.dll \
      $OBJ/*.obj $OBJ/$V/*.obj kernel32.lib user32.lib dxguid.lib winhttp.lib shell32.lib ole32.lib
  ls -la build/$V/$V.dll
done
