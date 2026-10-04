#!/usr/bin/env bash
# Puts edworld into the ed-lab clone (never Sjona's or Dunkan's install) and records the run directory.
# Variant "edhm" (default): game -> edworld d3d11.dll -> d3d11_edhm.dll (EDVR out of the chain).
# Variant "edvr": game -> edworld -> d3d11_edvr.dll (EDVR, which chains on to EDHM itself) — EDVR's draw census
#                 can then run in the same session (hotkey.dump_draws) for discovery.
# Backup of what was there goes to the run directory; tools/lab-revert.sh <run dir> puts it back.
# Does NOT start the game.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
VARIANT=${1:-edhm}
GAME=/ext/artur/ed-lab/game/Products/elite-dangerous-odyssey-64
RUN=/ext/artur/ed-lab/tests/edworld-$(date +%Y%m%d-%H%M%S)
[[ -f $HERE/build/d3d11.dll ]] || { echo "build first: tools/build.sh"; exit 1; }
[[ ! -L $GAME/d3d11.dll ]] || { echo "$GAME/d3d11.dll is a symlink; refusing to write through it"; exit 1; }
if pgrep -f '[E]liteDangerous64|[E]DLaunch' >/dev/null; then echo "a game is running; not touching files"; exit 1; fi
mkdir -p "$RUN/orig"
for f in d3d11.dll d3d11.pdb d3d11_edvr.dll edworld.ini; do
  [[ -e $GAME/$f || -L $GAME/$f ]] && cp -a "$GAME/$f" "$RUN/orig/"
done
case $VARIANT in
  edhm) NEXT=d3d11_edhm.dll ;;
  edvr) NEXT=d3d11_edvr.dll
        [[ -e $GAME/d3d11_edvr.dll ]] || cp -a "$GAME/d3d11.dll" "$GAME/d3d11_edvr.dll" ;;
  *) echo "variant: edhm | edvr"; exit 1 ;;
esac
cp "$HERE/build/d3d11.dll" "$HERE/build/d3d11.pdb" "$GAME/"
cat > "$GAME/edworld.ini" <<INI
; edworld (read-only panel observer), installed by tools/lab-install.sh $VARIANT
next = $NEXT
share = Z:\\dev\\shm\\edworld
log_interval_ms = 1000
INI
{
  echo "variant: $VARIANT"
  echo "edworld: $(git -C "$HERE" describe --tags --always --dirty)"
  echo "installed: $(date -Is)"
  sha256sum "$GAME/d3d11.dll"
  cat "$GAME/edworld.ini"
} > "$RUN/RUN.txt"
echo "installed ($VARIANT); run dir $RUN; log will be $GAME/edworld.log"
