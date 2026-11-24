#!/usr/bin/env bash
# What edworld costs the game, measured without it (not part of tools/test.sh: it takes a minute or two):
# - the graphics card: the clouds' volume as the game's draw does it (test/render_test.cc --bench) for the scenes of
#   two planets (by day and by night with the aurora at K 7, inside the layer, each also without the aurora) at the
#   game's picture size, under DXVK as the game is (its d3d11.dll and dxgi.dll copied from a game's prefix);
# - the render thread: a draw (VSSetShader + DrawIndexed) through edworld against one past it (test_app.exe with
#   EDWORLD_TEST_BENCH), under wine's own d3d11; one run varies by tens of percent, so it runs
#   EDWORLD_BENCH_CPU_RUNS times (5 unless set, 1 to 2 s each) and each number is the runs' median (each on its own:
#   the median "edworld" need not be the median "through" less the median "without").
# The numbers are written to <scratch>/bench-<UTC>.txt; compared with test/data/bench/<card>.txt when it is there
# (more than 15% slower: FAIL), made again with EDWORLD_BENCH_UPDATE=1.
# Usage: tools/bench.sh <build dir> <scratch dir> [<width> <height>]   (default 1920 1080)
# Environment: EDWORLD_WINEPREFIX; EDWORLD_DXVK_DIR: a folder with DXVK's d3d11.dll and dxgi.dll (a game prefix's
# drive_c/windows/system32); with no DISPLAY, WINE_USE_EGL=1 unless set
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$(cd "${1:?build dir}" && pwd)
SCR=${2:?scratch dir}
WIDTH=${3:-1920}
HEIGHT=${4:-1080}
[[ -n ${EDWORLD_WINEPREFIX:-} ]] || { echo "$0: set EDWORLD_WINEPREFIX" >&2; exit 1; }
[[ -n ${EDWORLD_DXVK_DIR:-} && -f $EDWORLD_DXVK_DIR/d3d11.dll && -f $EDWORLD_DXVK_DIR/dxgi.dll ]] ||
  { echo "$0: set EDWORLD_DXVK_DIR to a folder with DXVK's d3d11.dll and dxgi.dll" >&2; exit 1; }
for f in test/render_test.exe test/edworld_eht/test_app.exe edworld_eht/edworld_eht.dll; do
  [[ -f $BUILD/$f ]] || { echo "$0: no $BUILD/$f (build first)" >&2; exit 1; }
done
export WINEPREFIX=$EDWORLD_WINEPREFIX
# with no X server (Wayland only, e.g. another account's session) wine's OpenGL, which wined3d needs, comes only
# through EGL, off unless WINE_USE_EGL is set
[[ -n ${DISPLAY:-} ]] || export WINE_USE_EGL=${WINE_USE_EGL:-1}
RUN=$(mktemp -d "$SCR/bench.XXXXXX")
mkdir "$RUN/gpu" "$RUN/cpu"
cp "$BUILD/test/render_test.exe" "$EDWORLD_DXVK_DIR/d3d11.dll" "$EDWORLD_DXVK_DIR/dxgi.dll" "$RUN/gpu/"
cp "$BUILD/test/edworld_eht/test_app.exe" "$RUN/cpu/"
cp "$BUILD/edworld_eht/edworld_eht.dll" "$RUN/cpu/d3d11.dll"
(cd "$RUN/gpu" && WINEDEBUG=-all DXVK_LOG_LEVEL=none WINEDLLOVERRIDES="d3d11,dxgi=n" wine render_test.exe pictures --bench "$WIDTH" "$HEIGHT" > out.txt 2>&1)
CPU_RUNS=${EDWORLD_BENCH_CPU_RUNS:-5}
[[ $CPU_RUNS =~ ^[1-9][0-9]*$ ]] || { echo "$0: EDWORLD_BENCH_CPU_RUNS must be a whole number from 1" >&2; exit 1; }
for i in $(seq 1 "$CPU_RUNS"); do
  (cd "$RUN/cpu" && EDWORLD_TEST_BENCH=1 WINEDEBUG=-all WINEDLLOVERRIDES="d3d11=n,b" wine test_app.exe > "out-$i.txt" 2>&1) ||
    { echo "$0: the CPU run $i of $CPU_RUNS failed (exit $?), its output: $RUN/cpu/out-$i.txt" >&2; exit 1; }
done
# the median of each number over the runs, in the line's own form
grep -h '^bench cpu:' "$RUN"/cpu/out-*.txt | awk -v runs="$CPU_RUNS" '
  function median(a, n,    i, j, t) {
    for(i = 2; i <= n; ++i) for(j = i; j > 1 && a[j - 1] > a[j]; --j) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
    return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2 }
  # the line test_app.cc prints: "bench cpu: a draw (VSSetShader + DrawIndexed) <t> us through edworld, <w> us
  # without it, edworld <e> us" (wine writes it with CR LF); any other shape stops the bench instead of giving a median
  # of other words
  { sub(/\r$/, "") }
  NF != 18 || $9 != "us" || $13 != "us" || $18 != "us" || $8 !~ /^[0-9.]+$/ || $12 !~ /^[0-9.]+$/ || $17 !~ /^-?[0-9.]+$/ {
    print "bench cpu: a line not of the known shape: " $0 > "/dev/stderr"; bad = 1; exit 1 }
  { ++n; through[n] = $8; without[n] = $12; own[n] = $17 }
  END {
    if(bad) exit 1
    if(n != runs) { print "bench cpu: " n " lines from " runs " runs" > "/dev/stderr"; exit 1 }
    printf "bench cpu: a draw (VSSetShader + DrawIndexed) %.3f us through edworld, %.3f us without it, edworld %.3f us (median of %d runs)\n",
      median(through, n), median(without, n), median(own, n), n }' > "$RUN/cpu/out.txt"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$SCR/bench-$STAMP.txt"
{
  echo "edworld bench $STAMP, $(git -C "$HERE" describe --tags --always --dirty 2>/dev/null || echo '?'), ${WIDTH}x${HEIGHT}, DXVK $(set +o pipefail; strings "$EDWORLD_DXVK_DIR/d3d11.dll" | grep -m1 -E '^v[0-9]+\.[0-9]+')"
  grep -h -E '^bench (gpu|cpu):' "$RUN/gpu/out.txt" "$RUN/cpu/out.txt"
} > "$OUT"
cat "$OUT"
CARD=$(grep -m1 '^bench gpu: card' "$OUT" | sed -E 's/^bench gpu: card (.*) \(vendor.*/\1/; s/[^A-Za-z0-9]+/_/g')
REF="$HERE/test/data/bench/$CARD-${WIDTH}x${HEIGHT}.txt"
rc=0
if [[ -n ${EDWORLD_BENCH_UPDATE:-} ]]; then
  mkdir -p "$HERE/test/data/bench"
  cp "$OUT" "$REF"
  echo "--- bench: the reference made again: $REF"
elif [[ -f $REF ]]; then
  # each scene's median against the reference's: more than 15% slower is a FAIL
  awk 'FNR == NR { if($1 == "bench" && $2 == "gpu:" && $3 != "card") ref[$3 " " $4] = $7; next }
       $1 == "bench" && $2 == "gpu:" && $3 != "card" && ($3 " " $4) in ref {
         r = ref[$3 " " $4]; slow = $7 > r * 1.15
         printf "%s: bench: %s %s %s ms, the reference %s ms\n", slow ? "FAIL" : "ok  ", $3, $4, $7, r
         if(slow) bad = 1 }
       END { exit bad }' "$REF" "$OUT" || rc=1
else
  echo "--- bench: no reference for this card and size ($REF)"
fi
echo "=== bench: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED) ($OUT)"
exit $rc
