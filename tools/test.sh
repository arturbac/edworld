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
  [[ $(head -n 3 "$ini" | sed -n 3p) == ';compass = 0' ]] && [[ $(grep -c 'clouds_alpha =' "$ini") -eq 1 ]] && [[ $(grep -c 'compass =' "$ini") -eq 1 ]] && [[ $(grep -c 'status_file =' "$ini") -eq 1 ]] && grep -q '^status_file = C:\\x\\Status.json$' "$ini" && grep -q '^;clouds_cover = 0.4$' "$ini"
  ini_check "completed, the player's lines stay" $?
  set -e
}
run_variant() {  # $1 = edworld | edworld_eht; sets rc on failure
  local V=$1 RUN r
  RUN=$(mktemp -d "$SCR/$V-test.XXXXXX")
  cp "$BUILD/test/$V/test_app.exe" "$BUILD/test/fake_next.dll" test/data/edsm_factions_shinrarta.json "$RUN/"
  cp "$BUILD/$V/$V.dll" "$RUN/d3d11.dll"
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
  popd >/dev/null
}
run_variant edworld
run_variant edworld_eht
echo "=== overall: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED)"
exit $rc
