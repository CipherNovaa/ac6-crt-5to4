#!/usr/bin/env python3
# Set Armored Core 6's Steam LaunchOptions. MUST be run with Steam CLOSED
# (Steam rewrites localconfig.vdf on exit). Pass the launch string as argv[1]
# (use "" to clear). No quotes allowed in the value (VDF would break).
import os, sys, re, glob

# Steam keeps one localconfig.vdf per account under userdata/<account id>/. Set STEAM_LOCALCONFIG
# to pick one explicitly; otherwise the single one found is used.
F = os.environ.get("STEAM_LOCALCONFIG")
if not F:
    found = glob.glob(os.path.expanduser("~/.local/share/Steam/userdata/*/config/localconfig.vdf"))
    if len(found) != 1:
        sys.exit(f"found {len(found)} Steam localconfig.vdf files; set STEAM_LOCALCONFIG to the right one")
    F = found[0]
new = sys.argv[1] if len(sys.argv) > 1 else ""
if '"' in new:
    sys.exit("launch options must not contain double-quotes")

t = open(F, encoding="utf-8", errors="replace").read()
# locate the app block keyed exactly "1888160" and replace its LaunchOptions
out = []
i = 0
done = False
for m in re.finditer(r'"1888160"\s*\{', t):
    pass
# Simpler: find the 1888160 block start, then its LaunchOptions within ~2KB.
m = re.search(r'"1888160"\s*\{', t)
if not m:
    sys.exit("AC6 app block not found")
start = m.start()
block = t[start:start + 2000]
if re.search(r'"LaunchOptions"\s*"[^"]*"', block):
    block2 = re.sub(r'("LaunchOptions"\s*")[^"]*(")', r'\g<1>' + new + r'\g<2>', block, count=1)
else:
    # insert a LaunchOptions line right after the opening brace
    block2 = re.sub(r'("1888160"\s*\{\s*)', r'\g<1>\n\t\t\t\t\t"LaunchOptions"\t\t"' + new + '"', block, count=1)
t = t[:start] + block2 + t[start + 2000:]
open(F, "w", encoding="utf-8").write(t)
print("LaunchOptions set to:", repr(new))
