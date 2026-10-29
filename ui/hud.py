#!/usr/bin/env python3
"""
AC6 5:4 HUD position editor.  Moves WHOLE HUD containers in SCREEN PIXELS, rebuilds + deploys.
(Whole containers only -> elements never split apart, centered things stay centered.)
After a change, relaunch:  ui/crt-me3-launch.sh (or the launcher's CRT mode)

dx > 0 = move RIGHT,  dy > 0 = move DOWN.   1 screen pixel is handled internally.

  ./hud.py show                    # current offsets (screen px from vanilla)
  ./hud.py nudge <block> <dx> <dy> # add to offset, rebuild+deploy
  ./hud.py set   <block> <dx> <dy> # set absolute offset, rebuild+deploy
  ./hud.py reset [block]           # back to default(s)
  ./hud.py build                   # rebuild from current offsets

BLOCKS:  player (AP+EN+stagger+boost cluster)   ammo   compass   speed   alt
NOTE: 'player' holds both the bottom-left AP block AND the centered bars, so move it
DOWN only (sideways would off-center the bars).  The AP block sits at a natural left margin.

  example:  ./hud.py nudge ammo 0 10     # move ammo block down 10px
            ./hud.py nudge player 0 -8   # move whole player cluster up 8px
"""
import sys, os, json, subprocess, shutil
import xml.etree.ElementTree as ET

WS    = os.path.dirname(os.path.abspath(__file__))   # ui/ workspace (gfx/, cache/, tools/ live here, git-ignored)
VAN   = f"{WS}/gfx/vanilla/01_000_fe.gfx"
CACHE = f"{WS}/cache/hud_fe_vanilla.xml"
WORK  = f"{WS}/gfx/work/01_000_fe.gfx"
FFDEC = os.environ.get("AC6_FFDEC", f"{WS}/tools/ffdec/ffdec.sh")
GAME  = os.environ.get("AC6_GAME_DIR", os.path.expanduser("~/.local/share/Steam/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game"))
DEPLOY= f"{GAME}/mod/menu/01_000_fe.gfx"
OFFJS = f"{WS}/hud_offsets.json"
PXTW  = 30   # twips per screen pixel (fit-to-width 1280/1920 * 20)

# each block = list of (placement name, characterId).  'player' is the whole container (keeps EN
# bar/stagger/boost centered); 'ap' fine-adjusts ONLY the bottom-left chunk (all 6 pieces incl.
# the EXPANSION wrapper) on top of the container, so it can reach the corner without splitting.
BLOCKS = {
 "player": [("PlayerHUD","699")],
 "ap":     [("AP_Label","600"),("AP_Gauge","607"),("AP_NumValue","613"),
            ("Repair","578"),("WallHack","584"),("LimiterRelease","564")],  # AP+EXPANSION+REPAIR+SCAN
 "ammo":   [("BulletStockPanel","884")],
 "compass":[("RingRadar","936")],
 "speed":  [("Speedometer","982")],
 "alt":    [("Altimeter","976")],
 "boss":   [("BossGauges","731")],   # top-center boss/target HP bar (move UP out of the old 16:9 band)
}
# clean default 5:4 layout (screen px offsets from vanilla 16:9)
DEFAULTS = {"player":(0,190), "ap":(-60,0), "ammo":(53,175), "compass":(0,150),
            "speed":(0,0), "alt":(0,0), "boss":(0,-150)}

def load_off():
    o = {b:list(DEFAULTS.get(b,(0,0))) for b in BLOCKS}
    if os.path.exists(OFFJS):
        for k,v in json.load(open(OFFJS)).items():
            if k in o: o[k]=v
    return o

def save_off(o): json.dump(o, open(OFFJS,"w"), indent=1)

def ensure_cache():
    if not os.path.exists(CACHE):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        print("one-time: extracting vanilla HUD XML (~15s)...")
        subprocess.run([FFDEC,"-swf2xml",VAN,CACHE], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def build(off):
    ensure_cache()
    tree = ET.parse(CACHE); root = tree.getroot()
    want = {}
    for b in BLOCKS:
        for key in BLOCKS[b]:
            want[key]=(off[b][0]*PXTW, off[b][1]*PXTW)
    moved=set()
    for it in root.iter('item'):
        if not it.get('type','').startswith('PlaceObject'): continue
        key=(it.get('name'), it.get('characterId') or it.get('characterID'))
        if key in want:
            m=it.find('matrix')
            if m is None or m.get('translateX') is None: continue
            dx,dy=want[key]
            m.set('translateX',str(int(m.get('translateX'))+dx))
            m.set('translateY',str(int(m.get('translateY'))+dy)); moved.add(key)
    tmp="/tmp/hud_fe_edit.xml"; tree.write(tmp,encoding="UTF-8",xml_declaration=True)
    subprocess.run([FFDEC,"-xml2swf",tmp,WORK],check=True,
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    shutil.copy(WORK,DEPLOY)
    print(f"built + deployed ({len(moved)}/{len(want)} containers moved)")
    print("relaunch:  ui/crt-me3-launch.sh")

def show(off):
    print("block    offset(px)   dx=right dy=down")
    for b in BLOCKS:
        print(f"  {b:<8} ({off[b][0]:+4d},{off[b][1]:+4d})")

def main():
    a=sys.argv[1:]
    if not a or a[0] in ("-h","--help","help"): print(__doc__); return
    off=load_off(); cmd=a[0]
    if cmd=="show": show(off); return
    if cmd=="build": build(off); return
    if cmd=="reset":
        if len(a)>1 and a[1] in BLOCKS: off[a[1]]=list(DEFAULTS.get(a[1],(0,0)))
        else: off={b:list(DEFAULTS.get(b,(0,0))) for b in BLOCKS}
        save_off(off); build(off); return
    if cmd in ("set","nudge") and len(a)>=4:
        b=a[1]
        if b not in BLOCKS: print("unknown block:",b,"->",list(BLOCKS)); return
        dx,dy=int(a[2]),int(a[3])
        off[b]=[dx,dy] if cmd=="set" else [off[b][0]+dx, off[b][1]+dy]
        save_off(off); print(f"{b} -> {off[b]}"); build(off); return
    print("bad command. run ./hud.py help")

if __name__=="__main__": main()
