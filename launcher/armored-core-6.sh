#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# Armored Core VI launcher — rofi popup picks where/how to play:
#
#   CRT  : native 5:4 on the CRT. EXCLUSIVE FULLSCREEN 1280x1024, EAC DISABLED,
#          the dxgi.dll hook fills the 3D to 5:4 and me3 loads the reflowed
#          Scaleform UI/HUD. OFFLINE ONLY.
#
#   Main : vanilla, online-safe, on the main monitor with HDR + 120 Hz
#          (via gamescope, which is a display wrapper, NOT a game mod). EAC
#          ENABLED, every mod removed for the session, the genuine
#          start_protected_game.exe restored and md5-checked before launch.
#
# Switching modes rewrites the Steam launch options, so Steam is restarted each
# time. Written for Hyprland (Lua-config build: `hl.dsp.*` dispatchers).
#
# Configuration (environment, all optional):
#   AC6_GAME_DIR       the game's Game/ folder
#   AC6_PREFIX         the Proton prefix (compatdata/1888160/pfx)
#   AC6_STATE_DIR      where the genuine EAC exe + stripped mod files are parked
#   AC6_GENUINE_EAC_MD5  md5 of YOUR genuine start_protected_game.exe (see README)
#   AC6_CRT_OUTPUT     Hyprland output name of the CRT        (default HDMI-A-1)
#   AC6_CRT_MONITOR_ID Hyprland monitor id of the CRT         (default 0)
#   AC6_CRT_WORKSPACE  a workspace that lives on the CRT      (default 11)
#   AC6_ME3_DIR        me3 install (bin/me3 + bin/win64/)
# ─────────────────────────────────────────────────────────────────────────────
set -u

APPID=1888160
REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
GD="${AC6_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game}"
export AC6_GAME_DIR="$GD"
STATE="${AC6_STATE_DIR:-$HOME/.local/share/ac6-crt-5to4}"
SETCFG="$REPO/launcher/ac6-set-graphicsconfig.py"
SETOPT="$REPO/launcher/ac6-set-launchopts.py"
EXTRAS="$STATE/extras"
CRT_OUT="${AC6_CRT_OUTPUT:-HDMI-A-1}"
CRT_MON_ID="${AC6_CRT_MONITOR_ID:-0}"
CRT_WS="${AC6_CRT_WORKSPACE:-11}"
WIN_CLASS="steam_app_1888160"
# ── CRT 5:4 mod: a D3D12 hook DLL (dxgi.dll proxy) that fills the tube native 5:4 ──
# Fills 3D to full 5:4 (aspect-constant patch), removes the UI letterbox frame-bars
# (auto-detected by shader hash), keeps menus/garage correct. All behaviour is baked
# into the DLL — no config file needed. dxgi.dll forwards real exports to dxgi_proxy.dll
# (a frozen copy of the prefix's DXVK dxgi). BOTH must be removed for online/EAC.
HOOK_DLL="$REPO/hook/build/dxgi-ac6-crt.dll"   # production hook build (hook/build.sh; defaults baked in)
PROXY_DLL="$STATE/dxgi_proxy.dll"             # frozen copy of the prefix's real DXVK dxgi (see README)
GENUINE_EAC="$STATE/start_protected_game.exe.genuine"  # the real EAC launcher (kept OUT of Game/)
GENUINE_EAC_MD5="${AC6_GENUINE_EAC_MD5:-}"             # expected checksum of the genuine EAC exe
# ── 5:4 UI/HUD mod: edited Scaleform .gfx loaded by me3 (asset-override loader, Linux-native) ──
# me3 redirects the game's menu/HUD .gfx to <Game>/mod/menu/*.gfx (our 5:4-reflowed UI: the
# in-mission HUD pushed to the true 5:4 edges, crosshair left engine-perfect). me3 blocks online
# by default (extra safety). The mod .gfx are INERT without me3 (vanilla reads its own BHD archives),
# so a normal Steam launch (Main) never loads them. CRT runs the dxgi 3D hook AND me3 together.
ME3_DIR="${AC6_ME3_DIR:-$REPO/ui/tools/me3}"
ME3_PROFILE="$REPO/ui/ac6.me3"
MAIN_OPTS="ENABLE_HDR_WSI=1 gamescope --fullscreen -w 2560 -h 1440 -r 120 --hdr-enabled --hdr-debug-force-output --hdr-sdr-content-nits 600 -- env ENABLE_GAMESCOPE_WSI=1 DXVK_HDR=1 DISABLE_HDR_WSI=1 %command%"
# CRT: gamescope gives the game its own 1280x1024 (5:4) virtual display so the swapchain
# is 5:4 regardless of the real monitors → the main monitor keeps its mode. gamescope runs
# nested (a "gamescope" window Hyprland places); play_crt moves it to the CRT after launch
# (safe — the 5:4 buffer is gamescope's, not the monitor's). No -f: Hyprland fullscreens it.
CRT_OPTS="gamescope -w 1280 -h 1024 -W 1280 -H 1024 -r 60 -- %command%"

menu()   { rofi -dmenu -i -p "Armored Core 6" -theme-str 'window {width: 600px;}'; }
notify() { command -v notify-send >/dev/null && notify-send "Armored Core 6" "$1"; echo "$1"; }

kill_daemon() {   # legacy: early versions held the aspect with a ptrace daemon; clear stragglers
  for p in $(ps -eo pid,args 2>/dev/null | grep -E 'ac6-(viewport|aspect)-daemon\.py' | grep -v grep | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
}
kill_game() {
  # Kill ALL game instances, not just one. The CRT path runs the game as armoredcore6.exe (via me3);
  # the online path runs start_protected_game.exe. comm is truncated to 15 chars, so match by substring
  # and kill EVERY match — otherwise relaunches stack (old instances pile up and lag the game; a recurring
  # footgun, see the vault journal). The old code only killed the single highest-RSS 'start_protected'.
  for pid in $(ps -eo pid,comm 2>/dev/null | awk '/armoredcore6|start_protecte/{print $1}'); do kill -9 "$pid" 2>/dev/null; done
  # also stop me3 host/launcher + wineserver/gamescope/steam.exe (kill by comm, never pkill -f a self-matching pattern)
  for pid in $(pgrep -x me3) $(pgrep -x me3-launcher.exe) $(pgrep -x gamescope) $(pgrep -x wineserver) $(pgrep -x "steam.exe"); do kill -9 "$pid" 2>/dev/null; done
  sleep 2
}
shutdown_steam() {
  pgrep -x steam >/dev/null || return 0
  steam -shutdown >/dev/null 2>&1
  local i=0; until ! pgrep -x steam >/dev/null; do i=$((i+1)); [ $i -gt 25 ] && break; sleep 2; done
}
start_steam() {
  setsid steam -silent >/dev/null 2>&1 & disown
  local i=0; until pgrep -x steam >/dev/null; do i=$((i+1)); [ $i -gt 20 ] && break; sleep 2; done
  sleep 12
}
clean_mods() {            # make the game folder pristine (online-safe) — strips EVERY mod
  mkdir -p "$EXTRAS"
  # dxgi.dll = our hook; dxgi_proxy.dll = the renamed DXVK dxgi it forwards to.
  # BOTH must go for EAC/online (FromSoft is strict). Also clear our diag sidecars.
  for f in dxgi.dll dxgi_proxy.dll dxgi.dll.skoff dxgi.ini d3d11.dll d3d12.dll d3d9.dll winmm.dll \
           dinput8.dll version.dll start_protected_game.exe.eacbak; do
    [ -e "$GD/$f" ] && mv -f "$GD/$f" "$EXTRAS/" 2>/dev/null
  done
  rm -f "$GD/ac6ctl.txt" "$GD/ac6vp.log" "$GD/ac6cap.bin" "$GD/ac6asp.bin" 2>/dev/null
}
install_crt_mod() {       # put the 5:4 hook in place (offline only)
  cp -f "$HOOK_DLL"  "$GD/dxgi.dll"       || { notify "CRT hook DLL missing: $HOOK_DLL"; exit 1; }
  cp -f "$PROXY_DLL" "$GD/dxgi_proxy.dll" || { notify "CRT proxy DLL missing: $PROXY_DLL"; exit 1; }
  rm -f "$GD/ac6ctl.txt" "$GD/ac6vp.log" "$GD/ac6cap.bin" 2>/dev/null  # defaults are baked in
}
eac_off() {  # disable EAC for offline CRT: replace the launcher with a copy of the game exe
  cp -f "$GD/armoredcore6.exe" "$GD/start_protected_game.exe"
}
eac_on()  {  # restore the GENUINE EAC launcher from the external copy (online)
  [ -f "$GENUINE_EAC" ] || { notify "ABORT: genuine EAC exe missing ($GENUINE_EAC) — NOT going online."; exit 1; }
  cp -f "$GENUINE_EAC" "$GD/start_protected_game.exe"
}
# Hard gate: refuse to launch online unless the Game folder is provably vanilla.
assert_vanilla_online() {
  local bad=""
  # no injector/mod DLLs or our sidecars may remain
  for f in dxgi.dll dxgi_proxy.dll d3d11.dll d3d12.dll d3d9.dll winmm.dll dinput8.dll version.dll \
           dxgi.ini dxgi.dll.skoff ac6ctl.txt ac6vp.log ac6cap.bin ac6asp.bin \
           start_protected_game.exe.eacbak; do
    [ -e "$GD/$f" ] && bad="$bad $f"
  done
  # the EAC launcher must be the genuine binary, not the armoredcore6.exe copy
  local got; got=$(md5sum "$GD/start_protected_game.exe" 2>/dev/null | cut -d' ' -f1)
  [ -n "$GENUINE_EAC_MD5" ] && [ "$got" = "$GENUINE_EAC_MD5" ] || bad="$bad start_protected_game.exe(md5=$got!=genuine)"
  if [ -n "$bad" ]; then
    notify "ABORT online — non-vanilla file(s):$bad . Fix before going online (or Steam → Verify integrity)."
    exit 1
  fi
}
wait_window() { local i=0; until hyprctl clients -j 2>/dev/null | grep -q "$1"; do i=$((i+1)); [ $i -gt 80 ] && return 1; sleep 3; done; }

play_crt() {
  kill_daemon; kill_game; shutdown_steam   # kill_daemon clears any stale aspect daemons
  clean_mods                       # wipe everything first…
  install_crt_mod                  # …then drop in the 5:4 3D hook (dxgi.dll + dxgi_proxy.dll)
  eac_off
  python3 "$SETCFG" crt            # EXCLUSIVE FULLSCREEN 1280x1024 → 5:4 swapchain on the CRT
  python3 "$SETOPT" "%command%"    # plain launch (NO gamescope): me3 + fullscreen give 5:4 direct
  [ -x "$ME3_DIR/bin/me3" ] || { notify "me3 missing ($ME3_DIR/bin/me3) — can't load 5:4 UI mod."; exit 1; }
  [ -f "$ME3_PROFILE" ] || sed "s|@GAME_DIR@|$GD|" "$REPO/ui/ac6.me3.example" > "$ME3_PROFILE"
  start_steam
  notify "CRT: native 5:4 (5:4 UI/HUD via me3 + dxgi 3D hook), offline. Main monitor untouched. (Online disabled.)"
  # Pre-focus the CRT's workspace so the game window MAPS on the tube (flash-free).
  hyprctl dispatch "hl.dsp.focus({ workspace = $CRT_WS })" >/dev/null 2>&1; sleep 0.3
  # Launch via me3: redirects menu/HUD .gfx to <Game>/mod/menu/*.gfx; the dxgi hook still loads
  # (3D fill + bar removal) alongside. me3 blocks online by default. Backgrounded (me3 supervises).
  ( cd "$ME3_DIR" && setsid ./bin/me3 --windows-binaries-dir "$ME3_DIR/bin/win64" \
       launch -p "$ME3_PROFILE" --show-logos false >/dev/null 2>&1 & disown )
  # Best-effort: once the game window appears, ensure it's on the CRT + fullscreen (harmless no-op
  # if ws11 pre-focus already mapped it there). Match by title (me3 launches a direct game window).
  wait_window "ARMORED CORE" || { notify "Game window never appeared (me3 launch)."; return 1; }
  sleep 0.5
  local info addr mon
  info=$(hyprctl clients -j 2>/dev/null | python3 -c '
import sys,json
for c in json.load(sys.stdin):
    if "ARMORED CORE" in (c.get("title") or ""): print(c["address"], c["monitor"]); break')
  set -- $info; addr="${1:-}"; mon="${2:-}"
  if [ -n "$addr" ] && [ "$mon" != "$CRT_MON_ID" ]; then   # already on the CRT? only move if elsewhere
    hyprctl dispatch "hl.dsp.window.focus({ window = \"address:$addr\" })" >/dev/null 2>&1; sleep 0.2
    hyprctl dispatch 'hl.dsp.window.move({ monitor = "'"$CRT_OUT"'" })' >/dev/null 2>&1; sleep 0.3
    hyprctl dispatch 'hl.dsp.window.fullscreen()' >/dev/null 2>&1
  fi
}

play_main() {
  kill_daemon; kill_game; shutdown_steam
  clean_mods                       # strip every mod from the game folder
  eac_on                           # restore real EAC for online
  assert_vanilla_online            # HARD GATE: abort if any non-vanilla file / wrong EAC exe
  python3 "$SETCFG" main           # borderless 2560x1440 (16:9)
  python3 "$SETOPT" "$MAIN_OPTS"   # gamescope HDR + 120 Hz (display wrapper, not a mod)
  start_steam
  notify "Main: vanilla 16:9, HDR + 120 Hz, EAC ON, online-safe. (Set 120 FPS in-game once.)"
  setsid steam "steam://rungameid/$APPID" >/dev/null 2>&1 & disown
  # no routing, no daemon — leave it vanilla on the main monitor
}

CHOICE="$(printf '%s\n' \
  "󰍹  CRT — native 5:4 (offline, modded)" \
  "󰍺  Main — vanilla 16:9, HDR + 120Hz (online)" | menu)"
case "$CHOICE" in
  *CRT*)  play_crt ;;
  *Main*) play_main ;;
  *) exit 0 ;;
esac
