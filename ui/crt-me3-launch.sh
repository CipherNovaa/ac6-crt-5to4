#!/bin/bash
# Launch AC6 on the NEC CRT via me3 (loads <Game>/mod/menu/*.gfx 5:4 UI edits) alongside the
# dxgi 5:4 hook. OFFLINE ONLY (modded files = EAC ban risk; me3 also blocks online by default).
# Lap-2 UI experiment launcher. The dxgi hook reads <Game>/ac6ctl.txt (asppatch/autovp/capevery).
set -u
WS="$(cd "$(dirname "$0")" && pwd)"
GAME="${AC6_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game}"
ME3_DIR="${AC6_ME3_DIR:-$WS/tools/me3}"
CRT_WORKSPACE="${AC6_CRT_WORKSPACE:-11}"   # a Hyprland workspace that lives on the CRT output
# me3 profile: generated from the template on first run so it points at this machine's game dir.
[ -f "$WS/ac6.me3" ] || sed "s|@GAME_DIR@|$GAME|" "$WS/ac6.me3.example" > "$WS/ac6.me3"
cd "$ME3_DIR" || { echo "no me3 dir ($ME3_DIR)"; exit 1; }
# Pre-focus the CRT's workspace so the game window MAPS on the CRT, not flashed on the main monitor.
hyprctl dispatch "hl.dsp.focus({ workspace = $CRT_WORKSPACE })" >/dev/null 2>&1
sleep 0.3
echo "launching AC6 via me3 (offline, 5:4 UI mod + dxgi hook) on the CRT..."
exec ./bin/me3 --windows-binaries-dir "$PWD/bin/win64" launch -p "$WS/ac6.me3" --show-logos false
