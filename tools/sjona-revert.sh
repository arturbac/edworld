#!/usr/bin/env bash
# Puts back Sjona's d3d11.dll (EDHM) saved by sjona-install.sh; edworld's log and ini go to the run directory.
# Usage: sjona-revert.sh <run dir>   (the one sjona-install.sh printed)
set -euo pipefail
RUN=${1:?run dir printed by sjona-install.sh}
GAME="/ext/artur/.local/share/Steam/steamapps/common/Elite Dangerous/Products/elite-dangerous-odyssey-64"
if pgrep -f '[E]liteDangerous64|[E]DLaunch' >/dev/null; then echo "a game is running; nothing changed"; exit 1; fi
[[ -f $RUN/orig/d3d11.dll ]] || { echo "no $RUN/orig/d3d11.dll"; exit 1; }
cp -a "$RUN/orig/d3d11.dll" "$GAME/d3d11.dll"
if [[ -f $GAME/edworld.log ]]; then mv "$GAME/edworld.log" "$RUN/"; fi
if [[ -f $GAME/edworld.ini ]]; then mv "$GAME/edworld.ini" "$RUN/edworld.ini.used"; fi
sha256sum "$GAME/d3d11.dll" "$RUN/orig/d3d11.dll"
echo "reverted from $RUN"
