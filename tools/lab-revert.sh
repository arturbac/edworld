#!/usr/bin/env bash
# Puts back what tools/lab-install.sh saved in <run dir>/orig, and moves edworld's log into the run dir.
set -euo pipefail
RUN=${1:?run dir}
GAME=/ext/artur/ed-lab/game/Products/elite-dangerous-odyssey-64
if pgrep -f '[E]liteDangerous64|[E]DLaunch' >/dev/null; then echo "a game is running; not touching files"; exit 1; fi
[[ -d $RUN/orig ]] || { echo "no $RUN/orig"; exit 1; }
[[ -f $GAME/edworld.log ]] && mv "$GAME/edworld.log" "$RUN/"
cp -a "$RUN/orig/." "$GAME/"
[[ -e $RUN/orig/edworld.ini ]] || mv "$GAME/edworld.ini" "$RUN/edworld.ini.used"
echo "reverted from $RUN"
