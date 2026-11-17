#!/usr/bin/env bash
# Smoke test without the game: runs the build's test_app.exe under wine (warm prefix, below) with edworld's
# d3d11.dll as a native override (next = system d3d11, i.e. wine's own). Needs a CMake build first (the dlls and
# the test's programs, EDWORLD_TESTS on).
# Both builds, each copied in as d3d11.dll (its ini and log keep the build's name): edworld, then edworld_eht.
# Usage: tools/test.sh <build dir> <scratch dir>   (a new run directory is made under the scratch dir; nothing deleted)
# Environment: EDWORLD_WINEPREFIX: a wine prefix kept warm between runs
# (made on first use, reused; a fresh one each run cost minutes in wineboot).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$(cd "${1:?build dir}" && pwd)
SCR=${2:?scratch dir}
need() { [[ -n ${!1:-} ]] || { echo "$0: set $1 ($2)" >&2; exit 1; }; }
need EDWORLD_WINEPREFIX "a wine prefix kept between runs"
for f in edworld/edworld.dll edworld_eht/edworld_eht.dll test/fake_next.dll test/edworld/test_app.exe test/edworld_eht/test_app.exe; do
  [[ -f $BUILD/$f ]] || { echo "$0: no $BUILD/$f (build first)" >&2; exit 1; }
done
cd "$HERE"
export WINEPREFIX=$EDWORLD_WINEPREFIX
if [[ ! -f $WINEPREFIX/system.reg ]]; then
  mkdir -p "$WINEPREFIX"
  WINEDEBUG=-all wineboot -i >/dev/null 2>&1 || true
fi
# one wineserver for all the runs below and the next test within ten minutes
wineserver -p600 >/dev/null 2>&1 || true  # already running (e.g. from wineboot) is fine
rc=0
ini_run() { EDWORLD_TEST_INI_KEEP=1 WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe >/dev/null || echo "--- ini: test_app exit $?"; }
ini_check() {  # $1 = what, $2 = a condition's result
  echo "--- ini: $1: $([[ $2 -eq 0 ]] && echo ok || echo FAIL)"
  [[ $2 -eq 0 ]] || rc=1
}
ini_cases() {  # $1 = edworld | edworld_eht; in the run directory
  local ini=$1.ini before
  mv "$ini" "$ini.session"
  set +e
  ini_run
  grep -q '^;clouds_alpha = 0.45$' "$ini" && grep -q '^;clouds_volume = 1$' "$ini" && grep -qE '^;status_file = .+Status\.json' "$ini" && grep -q '^;dump_vs = ' "$ini" && ! grep -qvE '^(;.*)?\r?$' "$ini"
  ini_check "made when there is none, all comments" $?
  before=$(sha256sum "$ini")
  ini_run
  [[ $(sha256sum "$ini") == "$before" ]]
  ini_check "a second start changes nothing" $?
  printf 'clouds_alpha = 0.3\n; mine\n;compass = 0\nstatus_file = C:\\x\\Status.json' > "$ini"
  ini_run
  [[ $(head -n 3 "$ini" | sed -n 3p) == ';compass = 0' ]] && [[ $(grep -c 'clouds_alpha =' "$ini") -eq 1 ]] && [[ $(grep -c 'compass =' "$ini") -eq 1 ]] && [[ $(grep -c 'status_file =' "$ini") -eq 1 ]] && grep -q '^status_file = C:\\x\\Status.json$' "$ini" && grep -q '^;clouds_cover = 0.55$' "$ini"
  ini_check "completed, the player's lines stay" $?
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
  for what in draw-seh draw-cpp thread; do
    case $what in
      draw-seh) want='hooks: a fault caught \(exception 0xC0000005' ;;
      draw-cpp) want='hooks: a fault caught \(exception 0xE06D7363, a C\+\+ exception' ;;
      thread) want='thread state: a fault \(exception 0xE06D7363, a C\+\+ exception\), 1 of 8' ;;
    esac
    EDWORLD_TEST_FAULT=$what WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe > "fault-$what.out"
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
doc_case() {  # doc/settings.md is made by edworld_eht from its table of settings (it has every setting); in the run directory
  set +e
  EDWORLD_SETTINGS_DOC=settings.md ini_run
  cmp -s settings.md "$HERE/doc/settings.md"
  local same=$?
  ini_check "doc/settings.md up to date with the table" $same
  [[ $same -eq 0 ]] || echo "    to bring it up to date: cp $PWD/settings.md $HERE/doc/settings.md"
  set -e
}
run_variant() {  # $1 = edworld | edworld_eht; sets rc on failure
  local V=$1 RUN r
  RUN=$(mktemp -d "$SCR/$V-test.XXXXXX")
  cp "$BUILD/test/$V/test_app.exe" "$BUILD/test/fake_next.dll" test/data/edsm_factions_shinrarta.json "$RUN/"
  cp "$BUILD/$V/$V.dll" "$RUN/d3d11.dll"
  mkdir "$RUN/overlay"
  cp "$BUILD/test/test_foreign.dll" "$RUN/overlay/"
  echo "=== $V ($RUN)"
  pushd "$RUN" >/dev/null
  set +e
  EDWORLD_TEST_CATALOG=1 WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe
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
  ini_cases "$V"
  creator_case "$V"
  fault_case "$V"
  second_device_case "$V"
  [[ $V != edworld_eht ]] || doc_case
  popd >/dev/null
}
run_variant edworld
run_variant edworld_eht
echo "=== overall: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED)"
exit $rc
