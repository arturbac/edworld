#!/usr/bin/env bash
# ed_perf.sh <result.md> [seconds] [pid]: passive CPU profile of a running Elite Dangerous, split by module.
# Attaches perf to the game process for N seconds (default 10), snapshots /proc maps and
# per-thread CPU times, reads the EHT overlay's frame count for fps, then ed_perf_report.py writes the table.
# The raw data stays next to the result in <result>.raw/ (about 7 MB), so the table can be made again:
#   python3 -I ed_perf_report.py <result>.raw <other.md>
# Reads the game only; writes only <result.md> and <result>.raw/. Refuses to overwrite either.
# Two games running (Sjona and Dunkan): the pid has to be given; the error lists both.
set -euo pipefail
[[ $# -ge 1 && $# -le 3 ]] || { echo "usage: ed-perf <result.md> [seconds=10] [pid]" >&2; exit 2; }
HERE=$(dirname "$(readlink -f "$0")")
OUT_MD=$1
SECS=${2:-10}
[[ $SECS =~ ^[0-9]+$ && $SECS -ge 1 && $SECS -le 120 ]] || { echo "error: seconds: 1..120" >&2; exit 2; }
RAW="${OUT_MD%.md}.raw"
[[ -e $OUT_MD ]] && { echo "error: $OUT_MD already exists" >&2; exit 1; }
[[ -e $RAW ]] && { echo "error: $RAW already exists" >&2; exit 1; }
command -v perf >/dev/null || { echo "error: perf not found" >&2; exit 1; }

if [[ $# -eq 3 ]]; then
  PID=$3
  [[ $PID =~ ^[0-9]+$ ]] || { echo "error: pid: digits only" >&2; exit 2; }
else
  PIDS=$(pgrep -x 'EliteDangerous6' || true)
  if [[ -z $PIDS ]]; then
    echo "error: no EliteDangerous64 process" >&2
    exit 1
  fi
  if [[ $(wc -l <<<"$PIDS") -ne 1 ]]; then
    echo "error: more than one EliteDangerous64 process; give the pid as the third argument:" >&2
    for p in $PIDS; do
      echo "  $p  $(grep -o -m1 '[^ ]*/compatdata/359320/pfx' "/proc/$p/maps" || echo '?')" >&2
    done
    exit 1
  fi
  PID=$PIDS
fi
grep -q 'EliteDangerous64' "/proc/$PID/cmdline" 2>/dev/null || { echo "error: pid $PID is not EliteDangerous64" >&2; exit 1; }

mkdir "$RAW"
echo "pid $PID secs $SECS start $(date -Is)" > "$RAW/info.txt"
cp "/proc/$PID/maps" "$RAW/maps.txt"

snap() {
  for t in /proc/"$PID"/task/*; do
    printf '%s\t%s\t%s\n' "${t##*/}" "$(cat "$t/comm" 2>/dev/null || echo '?')" \
      "$(sed 's/.*) //' "$t/stat" 2>/dev/null | awk '{print $12+$13}')"
  done
}
echo "profiling pid $PID for $SECS s ..."
# the EHT overlay's frame count (written once a second); before and after give the fps of the window
FRAMES="$HOME/.local/share/elite_help_tool/captures/frames-$PID.txt"
frames() { if [[ -f $FRAMES ]]; then cat "$FRAMES"; fi; }
frames > "$RAW/frames-before.txt"
snap > "$RAW/cpu-before.tsv"
T0=$(date +%s.%N)
perf record -q -F 1999 -p "$PID" -o "$RAW/perf.data" -- sleep "$SECS" 2> "$RAW/record.err"
T1=$(date +%s.%N)
snap > "$RAW/cpu-after.tsv"
frames > "$RAW/frames-after.txt"
echo "wall $(awk "BEGIN{print $T1 - $T0}") clk_tck $(getconf CLK_TCK) end $(date -Is)" >> "$RAW/info.txt"

# edloader folder of this game: the plugins' folder's parent, read from the maps
PLUGIN=$(grep -o -m1 '/.*/edloader/plugins/' "$RAW/maps.txt" || true)
LOG="${PLUGIN%plugins/}logs/edworld_eht.log"
if [[ -n $PLUGIN && -f $LOG ]]; then
  grep -E 'frame [0-9]+:' "$LOG" | tail -n $((SECS + 5)) > "$RAW/edworld-frames.txt" || true
else
  : > "$RAW/edworld-frames.txt"
fi
perf script -i "$RAW/perf.data" -F comm,tid,ip 2>/dev/null > "$RAW/samples.txt"
python3 -I "$HERE/ed_perf_report.py" "$RAW" "$OUT_MD"
echo "table: $OUT_MD"
echo "raw data: $RAW"
