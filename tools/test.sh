#!/usr/bin/env bash
# Smoke test without the game: builds test/test_app.exe, runs it under wine (warm prefix, below) with edworld's
# d3d11.dll as a native override (next = system d3d11, i.e. wine's own). Needs tools/build.sh first.
# Both builds, each copied in as d3d11.dll (its ini and log keep the build's name): edworld, then edworld_eht.
# Usage: tools/test.sh <scratch dir>   (a new run directory is made under it; nothing deleted)
# Environment: MSVC_WINE_ENV, FXC (as tools/build.sh), EDWORLD_WINEPREFIX: a wine prefix kept warm between runs
# (made on first use, reused; a fresh one each run cost minutes in wineboot).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCR=${1:?scratch dir}
need() { [[ -n ${!1:-} ]] || { echo "$0: set $1 ($2)" >&2; exit 1; }; }
need MSVC_WINE_ENV "msvc-wine's env.sh"
need FXC "fxc.exe of a Windows SDK"
need EDWORLD_WINEPREFIX "a wine prefix kept between runs"
source "$MSVC_WINE_ENV"
cd "$HERE"
mkdir -p build/test
for s in panel_vs:vs_5_0 other_vs:vs_5_0 found_vs:vs_5_0 panel_ps:ps_5_0; do
  n=${s%%:*}; t=${s##*:}
  WINEDEBUG=-all wine "$FXC" /nologo /T $t /E main /Vn g_$n /Fh build/test/$n.h test/$n.hlsl >/dev/null
done
for V in edworld edworld_eht; do
  DEFS=()
  [[ $V == edworld_eht ]] && DEFS=(/DEDWORLD_EHT)
  mkdir -p build/test/$V
  cl /nologo /O2 /MT /std:c++latest /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS "${DEFS[@]}" \
     /I"build/test" /Fobuild/test/$V/ /Febuild/test/$V/test_app.exe test/test_app.cc /link d3d11.lib dxguid.lib >/dev/null
done
cl /nologo /O2 /MT /LD /std:c++latest /W4 /DWIN32_LEAN_AND_MEAN /Fobuild/test/ /Febuild/test/fake_next.dll \
   test/fake_next.cc /link /EXPORT:D3D11CreateDevice=fake_D3D11CreateDevice >/dev/null
export WINEPREFIX=$EDWORLD_WINEPREFIX
if [[ ! -f $WINEPREFIX/system.reg ]]; then
  mkdir -p "$WINEPREFIX"
  WINEDEBUG=-all wineboot -i >/dev/null 2>&1 || true
fi
# one wineserver for all the runs below and the next test within ten minutes
wineserver -p600 >/dev/null 2>&1 || true  # already running (e.g. from wineboot) is fine
rc=0
run_variant() {  # $1 = edworld | edworld_eht; sets rc on failure
  local V=$1 RUN r
  RUN=$(mktemp -d "$SCR/$V-test.XXXXXX")
  cp build/test/$V/test_app.exe build/test/fake_next.dll test/data/edsm_factions_shinrarta.json "$RUN/"
  cp build/$V/$V.dll "$RUN/d3d11.dll"
  echo "=== $V ($RUN)"
  pushd "$RUN" >/dev/null
  set +e
  WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe
  r=$?; [[ $r -eq 0 ]] || rc=1
  local sessions=2
  if [[ $V == edworld_eht ]]; then
    echo "--- with EDSM allowed: the data source knows the destination, so EDSM must not be asked"
    EDWORLD_TEST_EDSM=1 WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe | grep -E "PASSED|FAILED"
    sleep 2; grep -E "edsm:|from the data source" $V.log | tail -2
    sessions=3
  fi
  echo "--- chained through fake_next.dll (calls d3d11.dll by name)"
  EDWORLD_TEST_NEXT=fake_next.dll WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe
  r=$?; [[ $r -eq 0 ]] || rc=1
  set -e
  # sessions in one directory: all but the last set aside as <plugin>.<UTC>.log when the next began
  local aside
  aside=$(ls | grep -cE "^$V\.[0-9]{8}T[0-9]{6}Z\.log\$" || true)
  echo "--- sessions set aside: $aside (expected $((sessions - 1)))"
  [[ $aside -eq $((sessions - 1)) ]] || rc=1
  [[ -e $V.log && ! -e d3d11.ini && ! -e d3d11.log ]] || { echo "FAIL: log not named after the build, or files named after the dll"; rc=1; }
  echo "--- $V.log"
  head -40 $V.log 2>/dev/null
  popd >/dev/null
}
run_variant edworld
run_variant edworld_eht
echo "=== overall: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED)"
exit $rc
