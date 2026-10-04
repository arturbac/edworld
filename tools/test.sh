#!/usr/bin/env bash
# Smoke test without the game: builds test/test_app.exe, runs it under wine in a fresh prefix with edworld's
# d3d11.dll as a native override (next = system d3d11, i.e. wine's own). Needs tools/build.sh first.
# Usage: tools/test.sh <scratch dir>   (a new prefix and run directory are made under it; nothing deleted)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCR=${1:?scratch dir}
source /ext/artur/msvc-wine/env.sh
FXC="/ext/artur/msvc/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
cd "$HERE"
mkdir -p build/test
for s in panel_vs:vs_5_0 other_vs:vs_5_0 panel_ps:ps_5_0; do
  n=${s%%:*}; t=${s##*:}
  WINEDEBUG=-all wine "$FXC" /nologo /T $t /E main /Vn g_$n /Fh build/test/$n.h test/$n.hlsl >/dev/null
done
cl /nologo /O2 /MT /std:c++latest /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS \
   /I"build/test" /Fobuild/test/ /Febuild/test/test_app.exe test/test_app.cc /link d3d11.lib dxguid.lib >/dev/null
cl /nologo /O2 /MT /LD /std:c++latest /W4 /DWIN32_LEAN_AND_MEAN /Fobuild/test/ /Febuild/test/fake_next.dll \
   test/fake_next.cc /link /EXPORT:D3D11CreateDevice=fake_D3D11CreateDevice >/dev/null
RUN=$(mktemp -d "$SCR/edworld-test.XXXXXX")
cp build/test/test_app.exe build/test/fake_next.dll build/d3d11.dll "$RUN/"
export WINEPREFIX="$RUN/pfx"
WINEDEBUG=-all wineboot -i >/dev/null 2>&1 || true
cd "$RUN"
set +e
WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe
rc=$?
echo "--- chained through fake_next.dll (calls d3d11.dll by name)"
EDWORLD_TEST_NEXT=fake_next.dll WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe
rc2=$?
[[ $rc -eq 0 ]] && rc=$rc2
set -e
echo "--- edworld.log ($RUN)"
cat "$RUN/edworld.log" 2>/dev/null | head -40
exit $rc
