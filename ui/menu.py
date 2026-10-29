#!/usr/bin/env python3
"""
AC6 5:4 menu .gfx editor.  Generalizes hud.py to ANY menu file: reflow edge-anchored
groups to the true 5:4 edges, keep centered content centered.  Same proven pipeline as
hud.py (ffdec -swf2xml -> edit XML -> -xml2swf -> deploy to Game/mod/menu/), rebuilt from
a cached vanilla XML each time so edits never compound.

  ./menu.py <file> dump                 # root-level placements: KEY depth charId name tx ty screenY
  ./menu.py <file> stage54 [on|off]     # set stage 1920x1080 -> 1920x1536 (5:4); off = back to 16:9
  ./menu.py <file> nudge <key> <dx> <dy># add screen-px offset to a root placement, rebuild+deploy
  ./menu.py <file> set   <key> <dx> <dy># set absolute screen-px offset
  ./menu.py <file> cover <key>          # scale a bg placement to COVER 5:4 (1.4222x, recenter)
  ./menu.py <file> opaque <shapeId>     # force a DefineShape's fills fully opaque (alpha 255)
  ./menu.py <file> vfill <key>          # scale a root placement's Y by 1.4222 about canvas
                                        # centre => its 16:9 band content covers the full 5:4
  ./menu.py <file> deepvfill <spr> <d>  # same, for a placement nested at <sprite>:<depth>
  ./menu.py <file> clone <src> <into> <tx> <ty>  # place a copy of char <src> INSIDE sprite
                                        # <into> at sprite-local (tx,ty) TWIPS, behind its
                                        # children (backing plate for a moved title)
  ./menu.py <file> stretch <factor>     # LAST RESORT, gated: vertical stage stretch (e.g. 1.4222)
  ./menu.py <file> build                # rebuild from current offsets + deploy
  ./menu.py <file> reset                # drop the override (deletes deployed file -> vanilla)
  ./menu.py <file> show                 # show this file's stored ops

<file> = a name in gfx/vanilla/ (e.g. 01_garagetopmenu  or  01_garagetopmenu.gfx)
<key>  = a placement's name if it has one, else  d<depth>  (see `dump`).  dx>0=right, dy>0=down.

Screen map (showAll, fit-to-width): 1 screen px = 30 twips.
 16:9 stage in 1280x1024 -> content band screen_y = 152 + (twips/20)*0.6667  (152..872)
 5:4  stage in 1280x1024 -> full screen     screen_y =       (twips/20)*0.6667  (0..1024)
So after stage54, push footers DOWN and headers stay, to fill the new vertical room.

NEVER run stretch on 01_000_fe (the in-mission HUD) — it's refused.
"""
import sys, os, json, subprocess, shutil
import xml.etree.ElementTree as ET

WS     = os.path.dirname(os.path.abspath(__file__))   # ui/ workspace (gfx/, cache/, tools/ git-ignored)
VANDIR = f"{WS}/gfx/vanilla"
CACHED = f"{WS}/cache"          # cache/<file>.xml = vanilla swf2xml
WORKD  = f"{WS}/gfx/work"
OFFD   = f"{WS}/menu_offsets"   # menu_offsets/<file>.json
FFDEC  = os.environ.get("AC6_FFDEC", f"{WS}/tools/ffdec/ffdec.sh")
GAME   = os.environ.get("AC6_GAME_DIR", os.path.expanduser("~/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game"))
DEPLOYD= f"{GAME}/mod/menu"
PXTW   = 30        # twips per screen pixel (fit-to-width 1280/1920 * 20)
Y169   = 21600     # stage Ymax for 16:9 (1080px)
Y54    = 30720     # stage Ymax for 5:4  (1536px)
HUD    = "01_000_fe"   # the in-mission HUD — protected from stretch
STRETCH_SKIP = {"PlayerModelView", "PlayerModel", "ModelView", "PlayerModelView_2p"}  # 3D viewports: never stretch

def norm(name):
    return name[:-4] if name.endswith(".gfx") else name

def paths(file):
    f = norm(file)
    return dict(name=f, van=f"{VANDIR}/{f}.gfx", cache=f"{CACHED}/menu_{f}.xml",
                work=f"{WORKD}/{f}.gfx", off=f"{OFFD}/{f}.json", deploy=f"{DEPLOYD}/{f}.gfx")

def load_ops(p):
    d = {"stage": "16:9", "moves": {}, "covers": [], "stretch": 0, "deep": {}, "explode": {},
         "opaque": [], "vfill": [], "deepvfill": [], "clones": []}
    if os.path.exists(p["off"]):
        d.update(json.load(open(p["off"])))
    return d

def save_ops(p, ops):
    os.makedirs(OFFD, exist_ok=True)
    json.dump(ops, open(p["off"], "w"), indent=1)

def ensure_cache(p):
    if not os.path.exists(p["van"]):
        sys.exit(f"no vanilla file: {p['van']}")
    if not os.path.exists(p["cache"]):
        os.makedirs(CACHED, exist_ok=True)
        print(f"one-time: caching vanilla XML for {p['name']} ...")
        subprocess.run([FFDEC, "-swf2xml", p["van"], p["cache"]], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def root_places(root):
    """Yield (key, item, matrix) for PlaceObject* that are DIRECT children of <tags>."""
    tags = root.find("tags")
    if tags is None:
        return
    for it in list(tags):
        if not (it.get("type", "").startswith("PlaceObject")):
            continue
        m = it.find("matrix")
        nm = it.get("name")
        key = nm if nm else f"d{it.get('depth')}"
        yield key, it, m

def screen_y(twips_y, stage_ymax):
    px = twips_y / 20.0
    if stage_ymax >= Y54 - 1:          # 5:4 full-screen
        return px * 0.6667
    return 152 + px * 0.6667           # 16:9 band

def dump(p):
    ensure_cache(p)
    root = ET.parse(p["cache"]).getroot()
    dr = root.find("displayRect")
    print(f"{p['name']}  stage Xmax={dr.get('Xmax')} Ymax={dr.get('Ymax')}")
    print(f"{'KEY':<10}{'depth':>6}{'char':>6}  {'tx':>8}{'ty':>8}  {'scrY(16:9)':>11}  name")
    n = 0
    for key, it, m in root_places(root):
        tx = m.get("translateX") if m is not None else None
        ty = m.get("translateY") if m is not None else None
        sy = f"{screen_y(int(ty), Y169):.0f}" if ty not in (None,) else "-"
        print(f"{key:<10}{str(it.get('depth')):>6}{str(it.get('characterId')):>6}  "
              f"{str(tx):>8}{str(ty):>8}  {sy:>11}  {it.get('name') or ''}")
        n += 1
    print(f"-- {n} root placements --")

def build(p, ops):
    ensure_cache(p)
    tree = ET.parse(p["cache"]); root = tree.getroot()
    # 1) stage
    dr = root.find("displayRect")
    dr.set("Ymax", str(Y54 if ops["stage"] == "5:4" else Y169))
    # 2) per-placement moves + covers (root-level only)
    want = {k: (v[0]*PXTW, v[1]*PXTW) for k, v in ops["moves"].items()}
    covers = set(ops.get("covers", []))
    moved = []; covered = []
    for key, it, m in root_places(root):
        if key in want and m is not None and m.get("translateX") is not None:
            dx, dy = want[key]
            m.set("translateX", str(int(m.get("translateX")) + dx))
            m.set("translateY", str(int(m.get("translateY")) + dy))
            moved.append(key)
        if key in covers and m is not None:
            # cover 5:4: uniform 1.4222x (1536/1080), recenter horizontally (-8100 twips)
            m.set("hasScale", "true")   # ffdec drops scaleX/Y unless this is set
            m.set("scaleX", "1.4222"); m.set("scaleY", "1.4222")
            m.set("translateX", str(int(m.get("translateX") or 0) - 8100))
            covered.append(key)
    # 2b) NESTED moves inside a sprite (for backgrounds/frames bundled in frame sprites).
    #     deep   = move the placement(s) at <sprite>:<depth> by (dx,dy) screen px (all frames).
    #     explode= per sprite, shift every child placement above the Y midline UP, below DOWN
    #              (the frame case: top border/title-bg up, bottom footer-bg down, in one op).
    deep = ops.get("deep", {}); explode = ops.get("explode", {})
    deep_hits = 0; expl_hits = 0
    if deep or explode:
        spmap = {}
        for it in root.iter("item"):
            if it.get("type", "").startswith("DefineSprite"):
                spmap[it.get("spriteId") or it.get("characterId")] = it
        def children(sc):
            sp = spmap.get(str(sc)); sub = sp.find("subTags") if sp is not None else None
            return [it for it in (list(sub) if sub is not None else [])
                    if it.get("type", "").startswith("PlaceObject")]
        for sc, v in explode.items():
            up, dn = v[0], v[1]; thr = v[2] if len(v) > 2 else 10800
            for it in children(sc):
                m = it.find("matrix")
                if m is None or m.get("translateY") is None: continue
                ty = int(m.get("translateY"))
                m.set("translateY", str(ty + (-up if ty < thr else dn) * PXTW)); expl_hits += 1
        for key, (dx, dy) in deep.items():
            sc, dep = key.split(":")
            for it in children(sc):
                if it.get("depth") != dep: continue
                m = it.find("matrix")
                if m is None or m.get("translateX") is None: continue
                m.set("translateX", str(int(m.get("translateX")) + dx*PXTW))
                m.set("translateY", str(int(m.get("translateY")) + dy*PXTW)); deep_hits += 1
    # 2b2) vfill — vertical band-to-fullscreen scaling: scaleY x1.4222 about the 16:9
    #      canvas centre (10800 twips) so a band-covering layer (screen 152..872) covers the
    #      full 5:4 screen (0..1024). X untouched. Applied per-tag, so every animation
    #      frame's matrix for that key/depth stays consistent.
    VF = 1024.0/720.0            # 1.4222
    def _vfill_matrix(m):
        sy = float(m.get("scaleY") or 1)
        ty = int(m.get("translateY") or 0)
        m.set("hasScale", "true")
        m.set("scaleX", str(float(m.get("scaleX") or 1)))
        m.set("scaleY", str(sy * VF))
        m.set("translateY", str(int(VF*ty + (1-VF)*10800)))
    vfill = set(ops.get("vfill", []))
    vf_hits = 0
    if vfill:
        for key, it, m in root_places(root):
            if key in vfill and m is not None and m.get("translateY") is not None:
                _vfill_matrix(m); vf_hits += 1
    dvf = ops.get("deepvfill", [])
    if dvf:
        spmap2 = {}
        for it in root.iter("item"):
            if it.get("type", "").startswith("DefineSprite"):
                spmap2[it.get("spriteId") or it.get("characterId")] = it
        for keyspec in dvf:
            sc, dep = keyspec.split(":")
            sp = spmap2.get(str(sc)); sub = sp.find("subTags") if sp is not None else None
            for it in (list(sub) if sub is not None else []):
                if not it.get("type", "").startswith("PlaceObject"): continue
                if it.get("depth") != dep: continue
                m = it.find("matrix")
                if m is None or m.get("translateY") is None: continue
                _vfill_matrix(m); vf_hits += 1
    # 2d) clone — place an extra copy of an existing character INSIDE a sprite, behind its
    #     children (clone at depth 1, existing children bumped +1).  Use: a moved title
    #     carries its own copy of the backing plate, because the engine positions the
    #     vanilla plate independently and static edits can't move it.  tx/ty sprite-local twips.
    clones = ops.get("clones", [])
    cl_hits = 0
    if clones:
        import copy as _copy
        spmap3 = {}
        for it in root.iter("item"):
            if it.get("type", "").startswith("DefineSprite"):
                spmap3[it.get("spriteId") or it.get("characterId")] = it
        for c in clones:
            sp = spmap3.get(str(c["into"]))
            sub = sp.find("subTags") if sp is not None else None
            if sub is None: continue
            kids = [it for it in list(sub) if it.get("type", "").startswith("PlaceObject")]
            if not kids: continue
            tmpl = _copy.deepcopy(kids[0])          # schema-safe template
            for k2 in kids:                          # free depth 1 -> clone renders BEHIND
                k2.set("depth", str(int(k2.get("depth")) + 1))
            tmpl.set("characterId", str(c["src"]))
            tmpl.set("depth", "1")
            tmpl.set("placeFlagHasName", "false")
            tmpl.attrib.pop("name", None)
            m = tmpl.find("matrix")
            m.set("hasScale", "false"); m.set("hasRotate", "false")
            m.set("translateX", str(int(c["tx"]))); m.set("translateY", str(int(c["ty"])))
            sub.insert(0, tmpl)
            cl_hits += 1
    # 2c) opaque — force listed DefineShape fills to alpha 255 (e.g. a 50%-black hint-bar
    #     backing that must fully hide the shared shell's hint text stacked beneath it).
    opq = set(str(s) for s in ops.get("opaque", []))
    opq_hits = 0
    if opq:
        for it in root.iter("item"):
            if it.get("type", "").startswith("DefineShape") and str(it.get("shapeId")) in opq:
                for col in it.iter("color"):
                    if col.get("alpha") is not None:
                        col.set("alpha", "255"); opq_hits += 1
    # 3) stretch (LAST RESORT, per-file gated) — vertical scale of the 2D UI to fill 5:4.
    #    Scales each root placement's content by f ABOUT THE CANVAS CENTRE (10800 twips = scrY512),
    #    so the 16:9 band (scrY152..872) maps to the full screen (0..1024) at f=1024/720≈1.4222 —
    #    which also COVERS the shared shell's title/hint-bar that poke past the band in sub-screens.
    #    SKIPS 3D-viewport placements (PlayerModelView etc.) so the mech isn't distorted.
    f = float(ops.get("stretch", 0) or 0)
    if f > 0:
        if p["name"] == HUD:
            sys.exit("REFUSED: stretch on the in-mission HUD (01_000_fe) is forbidden.")
        # Make the stage 5:4 so the engine maps it to the full screen WITHOUT letterbox, then
        # scale ALL content vertically from the top (y=0) by f=1536/1080≈1.4222 so the 1080-tall
        # content fills the 1536 stage = the full 1024 screen. Masks scale with their content, so
        # nothing clips at the old band. X is untouched (pure vertical stretch = the accepted "pain").
        dr.set("Ymax", str(Y54))
        for key, it, m in root_places(root):
            if m is None or key in STRETCH_SKIP:
                continue
            m.set("hasScale", "true")   # ffdec drops scaleX/Y unless this is set
            m.set("scaleX", str(float(m.get("scaleX") or 1)))
            m.set("scaleY", str(float(m.get("scaleY") or 1) * f))
            m.set("translateY", str(int(float(m.get("translateY") or 0) * f)))
    # write + recompile + deploy
    os.makedirs(WORKD, exist_ok=True)
    tmp = f"/tmp/menu_{p['name']}_edit.xml"
    tree.write(tmp, encoding="UTF-8", xml_declaration=True)
    subprocess.run([FFDEC, "-xml2swf", tmp, p["work"]], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.makedirs(DEPLOYD, exist_ok=True)
    shutil.copy(p["work"], p["deploy"])
    print(f"built + deployed {p['name']}.gfx  (stage={ops['stage']}, "
          f"moved={len(moved)}/{len(want)}, deep={deep_hits}, explode={expl_hits}, "
          f"covers={len(covered)}, opaque={opq_hits}, vfill={vf_hits}, clones={cl_hits}, "
          f"stretch={f or 'off'})")
    if moved: print("  moved:", ", ".join(moved))
    print("relaunch:  ui/crt-me3-launch.sh")

def reset(p):
    for f in (p["off"],):
        if os.path.exists(f): os.remove(f)
    if os.path.exists(p["deploy"]):
        os.remove(p["deploy"]); print(f"removed deployed {p['name']}.gfx -> me3 serves vanilla")
    else:
        print(f"no deployed {p['name']}.gfx (already vanilla)")

def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help", "help"):
        print(__doc__); return
    p = paths(a[0]); cmd = a[1] if len(a) > 1 else "show"
    ops = load_ops(p)
    if cmd == "dump":  dump(p); return
    if cmd == "show":  print(json.dumps(ops, indent=1)); return
    if cmd == "build": build(p, ops); return
    if cmd == "reset": reset(p); return
    if cmd == "stage54":
        ops["stage"] = "16:9" if (len(a) > 2 and a[2] == "off") else "5:4"
        save_ops(p, ops); build(p, ops); return
    if cmd == "cover" and len(a) > 2:
        k = a[2]
        if k not in ops["covers"]: ops["covers"].append(k)
        save_ops(p, ops); build(p, ops); return
    if cmd == "opaque" and len(a) > 2:
        k = a[2]
        if k not in ops["opaque"]: ops["opaque"].append(k)
        save_ops(p, ops); build(p, ops); return
    if cmd == "vfill" and len(a) > 2:
        k = a[2]
        ops.setdefault("vfill", [])
        if k not in ops["vfill"]: ops["vfill"].append(k)
        save_ops(p, ops); build(p, ops); return
    if cmd == "deepvfill" and len(a) > 3:
        k = f"{a[2]}:{a[3]}"
        ops.setdefault("deepvfill", [])
        if k not in ops["deepvfill"]: ops["deepvfill"].append(k)
        save_ops(p, ops); build(p, ops); return
    if cmd == "clone" and len(a) >= 6:             # clone <srcChar> <intoSprite> <tx> <ty> twips
        c = {"src": int(a[2]), "into": int(a[3]), "tx": int(a[4]), "ty": int(a[5])}
        cl = ops.setdefault("clones", [])
        cl[:] = [x for x in cl if not (x["src"] == c["src"] and x["into"] == c["into"])]
        cl.append(c)
        save_ops(p, ops); build(p, ops); return
    if cmd == "stretch" and len(a) > 2:
        ops["stretch"] = float(a[2]); save_ops(p, ops); build(p, ops); return
    if cmd == "explode" and len(a) >= 4:           # explode <spriteChar> <up> <down> [thr]
        sc = a[2]; v = [int(a[3]), int(a[4])] + ([int(a[5])] if len(a) > 5 else [])
        ops["explode"][sc] = v; save_ops(p, ops); build(p, ops); return
    if cmd in ("deep", "deepset") and len(a) >= 6:  # deep <spriteChar> <depth> <dx> <dy>
        key = f"{a[2]}:{a[3]}"; dx, dy = int(a[4]), int(a[5])
        cur = ops["deep"].get(key, [0, 0])
        ops["deep"][key] = [dx, dy] if cmd == "deepset" else [cur[0]+dx, cur[1]+dy]
        save_ops(p, ops); print(f"{key} -> {ops['deep'][key]}"); build(p, ops); return
    if cmd in ("nudge", "set") and len(a) >= 5:
        k, dx, dy = a[2], int(a[3]), int(a[4])
        cur = ops["moves"].get(k, [0, 0])
        ops["moves"][k] = [dx, dy] if cmd == "set" else [cur[0]+dx, cur[1]+dy]
        save_ops(p, ops); print(f"{k} -> {ops['moves'][k]}"); build(p, ops); return
    print("bad command. run ./menu.py help")

if __name__ == "__main__":
    main()
