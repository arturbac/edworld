#!/usr/bin/env bash
# Puts edworld (this build) in front of EDHM in Sjona's Steam install - PRODUCTION, run by Artur himself.
# Chain afterwards: game -> edworld (d3d11.dll) -> d3d11_edhm.dll (EDHM, already there, immutable, untouched).
# The current d3d11.dll (EDHM) is copied into a new run directory first; sjona-revert.sh <run dir> puts it back.
# Usage: sjona-install.sh [patch]   (1 = emblem only, the default; 2 = test frame + emblem)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
PATCH=${1:-1}
GAME="/ext/artur/.local/share/Steam/steamapps/common/Elite Dangerous/Products/elite-dangerous-odyssey-64"
RUN=/ext/artur/ed-lab/tests/edworld-sjona-$(date +%Y%m%d-%H%M%S)
[[ -f $HERE/build/d3d11.dll ]] || { echo "no build: $HERE/tools/build.sh"; exit 1; }
[[ -f $GAME/d3d11_edhm.dll ]] || { echo "no d3d11_edhm.dll in the game dir - EDHM would drop out of the chain"; exit 1; }
if pgrep -f '[E]liteDangerous64|[E]DLaunch' >/dev/null; then echo "a game is running; nothing changed"; exit 1; fi
if grep -a -q "read-only d3d11 observer" "$GAME/d3d11.dll"; then
  echo "d3d11.dll there is already edworld - revert first (sjona-revert.sh <run dir>), so the copy kept is EDHM's"; exit 1
fi
mkdir -p "$RUN/orig"
cp -a "$GAME/d3d11.dll" "$RUN/orig/"
if [[ -e $GAME/edworld.ini ]]; then cp -a "$GAME/edworld.ini" "$RUN/orig/"; fi
cp "$HERE/build/d3d11.dll" "$GAME/d3d11.dll"
cat > "$GAME/edworld.ini" <<INI
; edworld, installed by sjona-install.sh ($RUN)
next = d3d11_edhm.dll
shm_dir = Z:\\dev\\shm\\eht
log_interval_ms = 1000
patch = $PATCH
edsm = 1
INI
{
  echo "edworld: $(git -C "$HERE" describe --always --dirty) (patch $PATCH)"
  echo "installed: $(date -Is)"
  sha256sum "$GAME/d3d11.dll" "$RUN/orig/d3d11.dll"
  cat "$GAME/edworld.ini"
} > "$RUN/RUN.txt"
echo "installed (patch $PATCH)"
echo "run dir: $RUN"
echo "log:     $GAME/edworld.log"
echo "revert:  $HERE/tools/sjona-revert.sh $RUN"
