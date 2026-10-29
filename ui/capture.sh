#!/bin/bash
# Capture AC6's current backbuffer via the dxgi hook (display-independent — lets Claude SEE
# frames). REQUIRES the rofi "CRT" launch (which deploys the dxgi hook); crt-me3-launch.sh
# alone has no hook. Writes ac6ctl.txt with capevery only (asppatch/autovp keep their baked
# defaults — load_ctl leaves absent keys untouched, so the 3D fill stays on). Grabs the latest
# dump and converts to a PNG.  Usage: ./capture.sh [out.png]
set -u
GAME="${AC6_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game}"
CAP="$GAME/ac6cap.bin"
HERE="$(cd "$(dirname "$0")" && pwd)"
CONV="$HERE/../hook/tools/capraw2png.py"
OUT="${1:-/tmp/ac6_capture.png}"

echo "capevery=120" > "$GAME/ac6ctl.txt"
before=$(stat -c %Y "$CAP" 2>/dev/null || echo 0)
echo "capture armed (capevery=120). waiting for a fresh backbuffer dump..."
for i in $(seq 1 40); do
  now=$(stat -c %Y "$CAP" 2>/dev/null || echo 0)
  [ "$now" != "$before" ] && [ "$now" != "0" ] && { echo "got a fresh dump."; break; }
  sleep 1
done
if [ ! -f "$CAP" ]; then
  echo "NO ac6cap.bin — the dxgi hook isn't capturing."
  echo "Launch via rofi -> CRT (deploys the hook), not crt-me3-launch.sh."
  exit 1
fi
python3 "$CONV" "$CAP" "$OUT" && echo "captured -> $OUT"
# leave capevery on for iterative grabs; run  echo capevery=0 > ac6ctl.txt  (or reset) when done.
