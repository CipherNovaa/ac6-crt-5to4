#!/usr/bin/env python3
# Set Armored Core 6's GraphicsConfig.xml (UTF-16-LE, no BOM) for a target mode.
#   crt  -> FULLSCREEN 1280x1024 (5:4, for the NEC CRT + aspect daemon)
#   main -> BORDERLESS 2560x1440 (16:9, vanilla on the main monitor)
import os, sys, re

PFX = os.environ.get("AC6_PREFIX", os.path.expanduser("~/.local/share/Steam/steamapps/compatdata/1888160/pfx"))
CFG = os.path.join(PFX, "drive_c/users/steamuser/AppData/Roaming/ArmoredCore6/GraphicsConfig.xml")

mode = sys.argv[1]
if mode == "crt":
    screen, w, h = "FULLSCREEN", 1280, 1024
elif mode == "main":
    screen, w, h = "BORDERLESS", 2560, 1440
elif mode == "wintest":
    # 1280x1024 WINDOW on the main desktop: same 5:4 buffer (reproduces the
    # 16:9 letterbox) without an exclusive mode change. For headless mod testing.
    screen, w, h = "WINDOW", 1280, 1024
else:
    sys.exit("usage: ac6-set-graphicsconfig.py crt|main|wintest")

t = open(CFG, encoding="utf-16-le").read()
t = re.sub(r"<ScreenMode>[A-Z]+</ScreenMode>", f"<ScreenMode>{screen}</ScreenMode>", t)
for tag in ("Resolution-WindowScreenWidth", "Resolution-FullScreenWidth", "Resolution-BorderlessScreenWidth"):
    t = re.sub(rf"<{tag}>\d+</{tag}>", f"<{tag}>{w}</{tag}>", t)
for tag in ("Resolution-WindowScreenHeight", "Resolution-FullScreenHeight", "Resolution-BorderlessScreenHeight"):
    t = re.sub(rf"<{tag}>\d+</{tag}>", f"<{tag}>{h}</{tag}>", t)
open(CFG, "w", encoding="utf-16-le").write(t)
print(f"GraphicsConfig -> {screen} {w}x{h}")
