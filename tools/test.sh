#!/usr/bin/env bash
# Smoke test without the game: runs the build's test_app.exe under wine (warm prefix, below) with edworld's
# d3d11.dll as a native override (next = system d3d11, i.e. wine's own). Needs a CMake build first (the dlls and
# the test's programs, EDWORLD_TESTS on).
# Both builds, each copied in as d3d11.dll (its ini and log keep the build's name): edworld, then edworld_eht.
# Last tools/doc.sh: the ini's rules and doc/settings.md with no device, compared with test/data/ini and doc/.
# Usage: tools/test.sh <build dir> <scratch dir>   (a new run directory is made under the scratch dir; nothing deleted)
# Environment: EDWORLD_WINEPREFIX: a wine prefix kept warm between runs (made on first use, reused; a fresh one each
# run cost minutes in wineboot), WINE_COMMON_TIMEOUT (seconds for one wine run, 600 unless set); with no DISPLAY,
# WINE_USE_EGL=1 unless set. Needs edplugin_common cloned in the checkout (its tools/wine-common.sh).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
COMMON=$HERE/edplugin_common/tools/wine-common.sh
[[ -f $COMMON ]] || { echo "$0: no $COMMON; in $HERE:" \
  "git clone https://github.com/arturbac/edplugin_common.git edplugin_common" >&2; exit 1; }
. "$COMMON"
BUILD=$(cd "${1:?build dir}" && pwd)
SCR=${2:?scratch dir}
for f in edworld/edworld.dll edworld_eht/edworld_eht.dll test/fake_next.dll test/edworld/test_app.exe test/edworld_eht/test_app.exe test/render_test.exe; do
  [[ -f $BUILD/$f ]] || { echo "$0: no $BUILD/$f (build first)" >&2; exit 1; }
done
cd "$HERE"
wine_common_prefix EDWORLD_WINEPREFIX
rc=0
ini_run() { wine_common_exec EDWORLD_TEST_INI_KEEP=1 WINEDLLOVERRIDES="d3d11=n,b" -- test_app.exe >/dev/null || echo "--- ini: test_app exit $?"; }
ini_check() {  # $1 = what, $2 = a condition's result
  echo "--- ini: $1: $([[ $2 -eq 0 ]] && echo ok || echo FAIL)"
  [[ $2 -eq 0 ]] || rc=1
}
# the ini made by the game's start (the rules of reading and completing it, with no device: tools/doc.sh)
ini_cases() {  # $1 = edworld | edworld_eht; in the run directory
  local ini=$1.ini
  mv "$ini" "$ini.session"
  set +e
  ini_run
  # a new ini's lines end in "\r\n"
  grep -q $'^;clouds_alpha = 0.45\r$' "$ini" && grep -q $'^;clouds_volume = 1\r$' "$ini" && grep -q $'^;status_file =\r$' "$ini" \
    && grep -q '^;dump_vs = ' "$ini" && ! grep -qvE $'^(;.*)?\r$' "$ini"
  ini_check "made when there is none, all comments" $?
  EDLOADER_CONFIG_DIR=. ini_run
  grep -q 'chain: next = d3d11.dll is edworld itself' "$1.log"
  ini_check "under edloader an empty next is d3d11.dll" $?
  set -e
}
creator_case() {  # $1 = edworld | edworld_eht; a device asked through a dll from another folder in the game's own call
  local ini=$1.ini
  mkdir -p pass
  cp "$BUILD/test/test_foreign.dll" pass/bdhkm64.dll
  set +e
  printf 'creator_pass =\n' > "$ini"
  EDWORLD_TEST_PASSER='pass\bdhkm64.dll' ini_run
  # starts within a second share a log, so only the last device line is the run's
  grep 'D3D11CreateDevice:' "$1.log" | tail -n 1 | grep -q 'asked by bdhkm64.dll from outside the game.s folder: not followed; .*creator_pass = bdhkm64.dll'
  ini_check "a device asked through another folder's dll is not followed, the log names creator_pass" $?
  printf 'creator_pass = other.dll, BDHKM64.dll\n' > "$ini"
  EDWORLD_TEST_PASSER='pass\bdhkm64.dll' ini_run
  grep 'D3D11CreateDevice:' "$1.log" | tail -n 1 | grep -q 'asked by test_app.exe (past bdhkm64.dll: creator_pass)$'
  ini_check "creator_pass passes the dll over: the device is the game's" $?
  : > "$ini"
  EDWORLD_TEST_PASSER='pass\bdhkm64.dll' ini_run
  grep 'D3D11CreateDevice:' "$1.log" | tail -n 1 | grep -q 'asked by test_app.exe (past bdhkm64.dll: creator_pass)$'
  ini_check "bdhkm64.dll passed over by default, no ini line needed" $?
  set -e
}
fault_case() {  # $1 = edworld | edworld_eht; a fault in a guard and in a thread is caught, logged, and the game goes on
  local what want
  set +e
  for what in draw-seh draw-inline draw-cpp thread; do
    case $what in
      # the plugin is d3d11.dll here (the module's file name, not $1.dll)
      draw-seh | draw-inline) want='hooks: an access violation writing 0x0 at d3d11\.dll\+0x[0-9A-F]+ \(1 of 8\)' ;;
      draw-cpp) want='hooks: a C\+\+ exception \(EDWORLD_TEST_FAULT\) \(1 of 8\)' ;;
      thread) want='thread state: a C\+\+ exception \(EDWORLD_TEST_FAULT\) \(1 of 8\)' ;;
    esac
    wine_common_exec EDWORLD_TEST_FAULT=$what WINEDLLOVERRIDES="d3d11=n,b" -- test_app.exe > "fault-$what.out"
    local r=$?
    sleep 2  # the thread's line comes after its restart's second
    grep -qE "$want" "$1.log"
    local logged=$?
    # the state thread starts again a second later: test_app's checks of the jump panel run before it reads Status.json,
    # so they may fail (exit 1); the process must still end by itself, not by a crash
    local most=0; [[ $what == thread ]] && most=1
    ini_check "a fault ($what) caught and logged, test_app exit $r" $(( logged != 0 || r > most ))
  done
  set -e
}
second_device_case() {  # $1 = edworld | edworld_eht; a second device of the game is not followed, and said so
  set +e
  : > "$1.ini"
  EDWORLD_TEST_SECOND_DEVICE=1 ini_run
  grep -q 'attach: device .* is a second device of the game; not followed, edworld stays with the first' "$1.log"
  ini_check "a second device not followed, the log says so" $?
  set -e
}
chain_case() {  # $1 = edworld | edworld_eht; a 3Dmigoto (by its version resource) before edworld in the chain
  set +e
  : > "$1.ini"
  [[ -s $1.log ]] && ! grep -h 'warning: dll before edworld' "$1".*log | grep -q .
  ini_check "no dll before edworld said while none was" $?
  cp "$BUILD/test/fake_migoto.dll" .
  EDWORLD_TEST_PASSER='fake_migoto.dll' ini_run
  grep 'warning: dll before edworld in the chain: ' "$1.log" | grep 'fake_migoto.dll' | grep -q 'product "XXMI DLL (3Dmigoto)"'
  ini_check "the dll before edworld named with its version resource" $?
  if [[ $1 == edworld_eht ]]; then
    grep -q 'edworld_eht: off for this session: 3Dmigoto (fake_migoto.dll) is before it in the chain' "$1.log" \
      && grep -q 'edworld_eht off for this session, only its corner.s line drawn' "$1.log"
    ini_check "edworld_eht off with 3Dmigoto before it" $?
  else
    ! grep -q 'off for this session' "$1.log"
    ini_check "edworld goes on with 3Dmigoto before it" $?
  fi
  set -e
}
run_variant() {  # $1 = edworld | edworld_eht; sets rc on failure
  local V=$1 RUN r
  RUN=$(wine_common_run_dir "$SCR" "$V-test")
  cp "$BUILD/test/$V/test_app.exe" "$BUILD/test/fake_next.dll" test/data/edsm_factions_shinrarta.json "$RUN/"
  cp "$BUILD/$V/$V.dll" "$RUN/d3d11.dll"
  mkdir "$RUN/overlay"
  cp "$BUILD/test/test_foreign.dll" "$RUN/overlay/"
  echo "=== $V ($RUN)"
  pushd "$RUN" >/dev/null
  set +e
  wine_common_exec EDWORLD_TEST_CATALOG=1 WINEDLLOVERRIDES="d3d11=n,b" -- test_app.exe
  r=$?; [[ $r -eq 0 ]] || rc=1
  local sessions=2
  if [[ $V == edworld_eht ]]; then
    echo "--- with EDSM allowed: the data source knows the destination, so EDSM must not be asked"
    wine_common_exec EDWORLD_TEST_EDSM=1 WINEDLLOVERRIDES="d3d11=n,b" -- test_app.exe | grep -E "PASSED|FAILED"
    sleep 2; grep -E "edsm:|from the data source" $V.log | tail -2
    sessions=3
  fi
  echo "--- chained through fake_next.dll (calls d3d11.dll by name)"
  wine_common_exec EDWORLD_TEST_NEXT=fake_next.dll WINEDLLOVERRIDES="d3d11=n,b" -- test_app.exe
  r=$?; [[ $r -eq 0 ]] || rc=1
  set -e
  # sessions in one directory: all but the last set aside as <plugin>\<plugin>.<UTC>.log when the next began
  # (edplugin_common's log), none beside the log
  local aside beside
  aside=$({ ls "$V" 2> /dev/null || true; } | grep -cE "^$V\.[0-9]{8}T[0-9]{6}Z\.log\$" || true)
  beside=$(ls | grep -cE "^$V\.[0-9]{8}T[0-9]{6}Z\.log\$" || true)
  echo "--- sessions set aside in $V/: $aside (expected $((sessions - 1))), beside the log: $beside (expected 0)"
  [[ $aside -eq $((sessions - 1)) && $beside -eq 0 ]] || rc=1
  [[ -e $V.log && ! -e d3d11.ini && ! -e d3d11.log ]] || { echo "FAIL: log not named after the build, or files named after the dll"; rc=1; }
  echo "--- $V.log"
  head -40 $V.log 2>/dev/null
  ini_cases "$V"
  creator_case "$V"
  fault_case "$V"
  second_device_case "$V"
  chain_case "$V"
  popd >/dev/null
}
# the clouds' volume shader drawn for scenes of several atmospheres (test/render_test.cc), with the system d3d11 (no
# edworld in its folder), compared with test/data/render/*.png; EDWORLD_RENDER_UPDATE=1 makes those again
render_case() {
  local RUN r
  RUN=$(wine_common_run_dir "$SCR" render-test)
  cp "$BUILD/test/render_test.exe" "$RUN/"
  mkdir -p "$RUN/reference"
  cp test/data/render/*.png "$RUN/reference/" 2>/dev/null || true
  set +e
  wine_common_run "$RUN" "$RUN/render.out" -- render_test.exe pictures reference ${EDWORLD_RENDER_UPDATE:+--update}
  r=$?
  set -e
  cat "$RUN/render.out"
  if [[ -n ${EDWORLD_RENDER_UPDATE:-} && $r -eq 0 ]]; then
    mkdir -p test/data/render
    cp "$RUN"/reference/*.png test/data/render/
    echo "--- render: references made again in test/data/render"
  fi
  echo "--- render: pictures in $RUN/pictures: $([[ $r -eq 0 ]] && echo ok || echo FAIL)"
  [[ $r -eq 0 ]] || rc=1
}

run_variant edworld
run_variant edworld_eht
render_case
echo "--- tools/doc.sh: the ini's rules and doc/settings.md with no device"
"$HERE/tools/doc.sh" "$BUILD" "$SCR" || rc=1
echo "=== overall: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED)"
exit $rc
