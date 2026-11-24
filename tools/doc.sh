#!/usr/bin/env bash
# edworld's settings without the game, with no device and no drawing: test/doc_app.cc under wine loads a build
# (edworld.dll, edworld_eht.dll) and calls its export edworld_write_settings_doc, which makes the log and the ini as at
# the game's start and, with EDWORLD_SETTINGS_DOC set, doc/settings.md. Checked for each build, against
# test/data/ini/<build>_*: a fresh ini, and a second start that leaves it as it is; an ini already there keeps its
# lines and gets the settings it does not name added at its end; an ini of wrong lines gives the settings' log lines
# <build>_bad_log.txt; edworld_eht's doc is doc/settings.md, and each setting of its ini is a row of the doc with the
# same default.
# Usage: tools/doc.sh <build dir> <scratch dir>; environment: EDWORLD_WINEPREFIX (a wine prefix kept between runs),
# WINE_COMMON_TIMEOUT (seconds for one wine run, 600 unless set). Needs edplugin_common cloned in the checkout (its
# tools/wine-common.sh). tools/test.sh runs it too.
set -euo pipefail
# the shell's environment would move the runs' files out of their folders: edloader's folders for the ini and the
# log, and the doc's file, set below for the doc's run only
unset EDLOADER_CONFIG_DIR EDLOADER_LOG_DIR EDWORLD_SETTINGS_DOC
HERE=$(cd "$(dirname "$0")/.." && pwd)
COMMON=$HERE/edplugin_common/tools/wine-common.sh
[[ -f $COMMON ]] || { echo "$0: no $COMMON; in $HERE:" \
  "git clone https://github.com/arturbac/edplugin_common.git edplugin_common" >&2; exit 1; }
. "$COMMON"
BUILD=$(cd "${1:?build dir}" && pwd)
SCR=${2:?scratch dir}
for f in edworld/edworld.dll edworld_eht/edworld_eht.dll test/edworld_doc.exe; do
  [[ -f $BUILD/$f ]] || { echo "$0: no $BUILD/$f (build first)" >&2; exit 1; }
done
DATA=$HERE/test/data/ini
wine_common_prefix EDWORLD_WINEPREFIX
RUN=$(wine_common_run_dir "$SCR" edworld-doc)
rc=0
check() {  # <0 = ok> <what>
  if [[ $1 -eq 0 ]]; then echo "ok  : $2"; else echo "FAIL: $2"; rc=1; fi
}
# the program in a folder; 0 when the export said done and the process ended with 0, or with 137: wine at times kills
# the process at its exit (SIGKILL) once the system d3d11 is loaded, after the export said done (4 of 12 runs,
# 2026-11-24; the files are whole by then). Those runs are counted and said at the end; any other code is a failure
killed=0
export_run() {  # <dir> <build> <output file> [VAR=value ...]
  local dir=$1 build=$2 out=$3 code
  shift 3
  wine_common_run "$dir" "$out" "$@" -- edworld_doc.exe "$build.dll"
  code=$?
  grep -q '^ok  : edworld_write_settings_doc' "$out" || return 1
  [[ $code -ne 137 ]] || { killed=$((killed + 1)); return 0; }
  [[ $code -eq 0 ]] || echo "    edworld_doc.exe ended with $code after the export said done ($out)"
  return $code
}
# a case: its own folder with the build and the program, the ini given beforehand (printf's %b) or none
doc_run() {  # <build> <name> [the ini's text]
  local dir=$RUN/$1-$2
  mkdir "$dir"
  cp "$BUILD/$1/$1.dll" "$BUILD/test/edworld_doc.exe" "$dir/"
  [[ $# -lt 3 ]] || printf '%b' "$3" > "$dir/$1.ini"
  export_run "$dir" "$1" "$dir/out.txt"
}
# the settings' log lines: the log's settings lines without their time and without the ini's path
settings_lines() {  # <build> <case>
  grep 'settings: ' "$RUN/$1-$2/$1.log" | cut -d' ' -f2- | grep -v '\.ini' | tr -d '\r'
}
# the player's lines of the completed case, the last without a line end (edworld adds one before its own)
PLAYER='clouds_alpha = 0.3\n; mine\n;compass = 0\nstatus_file = C:\\x\\Status.json'
# wrong lines and the lines the rules of one table change (the compatibility table of the settings' rules)
BAD='clouds_alpha = 0,45\nclouds = 1 ; on\naurora_gain = nan\ncluds = 1\nlog_interval_ms = 99999999999\n'
BAD+='clouds_volume = 1;on\nwatch_vs = 81216C77F90DEDD6, 0x0123, nothing\ndump_vs += ABCDEF0123456789\n'
BAD+='share = Z:\\dev\\shm\\x\nclouds_radius = 2\nshare = Z:\\other\n'
BAD+='creator_pass = a.dll, b.dll ; mine\nnext = D:\\Games #2\\d3d11_edhm.dll\n'
BAD+='station_vs = 0\npatch_surface = 2200 x 1800\nindependent_colour = 1000000\nlist += 1\nno equals\n'
set +e
for V in edworld edworld_eht; do
  doc_run $V fresh
  check $? "$V fresh: the export ran"
  cmp -s "$RUN/$V-fresh/$V.ini" "$DATA/${V}_fresh.ini"
  check $? "$V fresh: the ini is test/data/ini/${V}_fresh.ini"
  before=$(sha256sum < "$RUN/$V-fresh/$V.ini")
  export_run "$RUN/$V-fresh" "$V" "$RUN/$V-fresh/out-second.txt"
  [[ $? -eq 0 && $(sha256sum < "$RUN/$V-fresh/$V.ini") == "$before" ]]
  check $? "$V fresh: a second start leaves it as it is"
  doc_run $V completed "$PLAYER"
  check $? "$V completed: the export ran"
  cmp -s "$RUN/$V-completed/$V.ini" "$DATA/${V}_completed.ini"
  check $? "$V completed: the ini is test/data/ini/${V}_completed.ini"
  head -c "$(printf '%b' "$PLAYER" | wc -c)" "$RUN/$V-completed/$V.ini" | cmp -s - <(printf '%b' "$PLAYER")
  check $? "$V completed: the player's lines byte for byte at the start of the file"
  doc_run $V bad "$BAD"
  check $? "$V bad: the export ran"
  settings_lines $V bad | cmp -s - "$DATA/${V}_bad_log.txt"
  check $? "$V bad: the settings' log lines are test/data/ini/${V}_bad_log.txt"
done
# the doc from edworld_eht, which has every setting
mkdir "$RUN/doc"
cp "$BUILD/edworld_eht/edworld_eht.dll" "$BUILD/test/edworld_doc.exe" "$RUN/doc/"
export_run "$RUN/doc" edworld_eht "$RUN/doc/out.txt" EDWORLD_SETTINGS_DOC=settings.md
check $? "doc: the export ran"
cmp -s "$RUN/doc/settings.md" "$HERE/doc/settings.md"
same=$?
check $same "doc: doc/settings.md up to date with the table"
[[ $same -eq 0 ]] || echo "    to bring it up to date: cp '$RUN/doc/settings.md' '$HERE/doc/settings.md'"
# the ini's settings with their defaults (";name = value", ";name =" for an empty one) and the doc's rows ("| `name` |
# `value` |", "| `name` | empty |"), each sorted, are the same pairs
diff <(sed -n 's/^;\([a-z_0-9]*\) =[ ]\{0,1\}\([^\r]*\)\r*$/\1=\2/p' "$RUN/edworld_eht-fresh/edworld_eht.ini" | sort) \
     <(sed -n -e 's/^| `\([a-z_0-9]*\)` | `\([^`]*\)` |.*/\1=\2/p' -e 's/^| `\([a-z_0-9]*\)` | empty |.*/\1=/p' "$RUN/doc/settings.md" \
         | sed 's/\\|/|/g' | sort) > /dev/null
check $? "doc: every setting of edworld_eht's ini a row of the doc with its default, and no other"
set -e
echo "--- doc: wine killed edworld_doc.exe at its exit, after the export said done, in $killed of 9 runs"
echo "=== doc: $([[ $rc -eq 0 ]] && echo PASSED || echo FAILED) (run directory $RUN)"
exit $rc
