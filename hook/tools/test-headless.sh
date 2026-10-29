#!/bin/bash
# Headless RE harness for the AC6 vert+ mod. Runs the game on a virtual 1280x1024
# 5:4 output (CRT can be off), drives to a 3D scene, dumps the hook log. The game
# renders internally (the hook log captures viewports + constant buffers) even
# though the virtual output may not present visible frames. Non-disruptive to the
# main monitor. Usage: ./test-headless.sh [seconds_to_drive]
set -u
GD="${AC6_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game}"
DRIVE="${1:-60}"

say(){ echo "[test] $*"; }

# kill any running game
g=$(ps -eo pid,comm --sort=-rss | awk '/start_protected/{print $1;exit}'); [ -n "$g" ] && kill -9 "$g" 2>/dev/null
sleep 2

# ensure headless 1280x1024 exists
if ! hyprctl monitors -j 2>/dev/null | grep -q HEADLESS; then
  hyprctl output create headless >/dev/null 2>&1; sleep 1
fi
HLN=$(hyprctl monitors -j 2>/dev/null | python3 -c 'import sys,json
for m in json.load(sys.stdin):
  if "HEADLESS" in m["name"]: print(m["name"]);break')
hyprctl dispatch "hl.monitor({ output = \"$HLN\", mode = \"1280x1024@60\", position = \"3840x0\", scale = 1 })" >/dev/null 2>&1
sleep 1
say "headless = $HLN $(hyprctl monitors -j 2>/dev/null | python3 -c 'import sys,json
for m in json.load(sys.stdin):
  if "HEADLESS" in m["name"]: print(str(m["width"])+"x"+str(m["height"]));break')"

# launch
pgrep -x ydotoold >/dev/null || { setsid ydotoold >/dev/null 2>&1 & disown; sleep 2; }
rm -f "$GD/ac6vp.log"
setsid steam "steam://rungameid/1888160" >/dev/null 2>&1 & disown
say "waiting for window..."
i=0; until hyprctl clients -j 2>/dev/null | grep -q steam_app_1888160; do i=$((i+1)); [ $i -gt 60 ] && { say "no window"; exit 1; }; sleep 3; done

# move game onto the headless output + fullscreen (no rule edits needed)
addr=$(hyprctl clients -j 2>/dev/null | python3 -c 'import sys,json
for c in json.load(sys.stdin):
  if "1888160" in str(c.get("class","")): print(c["address"]);break')
say "window $addr -> $HLN"
hyprctl dispatch "hl.dsp.window.move({ address = \"$addr\", monitor = \"$HLN\" })" >/dev/null 2>&1; sleep 0.5
# show headless's workspace where the window is, drive with returns
SEND="hl.dsp.send_shortcut({ mods = 0, key = \"return\", window = \"class:steam_app_1888160\" })"
say "driving to a 3D scene (~${DRIVE}s)..."
end=$((SECONDS+DRIVE))
while [ $SECONDS -lt $end ]; do sleep 3; hyprctl dispatch "$SEND" >/dev/null 2>&1; done

say "=== hook log ==="
cat "$GD/ac6vp.log" 2>/dev/null
say "=== game alive? ==="
ps -eo pid,comm --sort=-rss | awk '/start_protected/{print "pid",$1; f=1} END{if(!f)print "GONE"}'
