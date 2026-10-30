# ac6-crt-5to4

Armored Core VI on a 5:4 CRT (1280x1024), filled edge to edge with correct geometry: a D3D12 hook makes the engine render the 3D scene natively at 5:4, and a Scaleform toolchain moves the HUD and garage menus out to the tube's real edges.

**Status:** working and in daily use on Linux/Proton. The 3D world (missions, garage, cutscenes) fills 5:4 with a round reticle. The in-mission HUD is reflowed to the 5:4 edges. The garage menus (17 screens) read native 5:4 and were checked on the CRT. **Still to do:** check the mission-result and photo-mode screens in a mission, visit the Archive screen (its edits follow the family pattern but haven't been seen), and package for Nexus Mods. A brief black fade box on garage entry is diagnosed but not fixed.

<!-- MEDIA: side-by-side photo of the CRT in a mission. Left: vanilla (16:9 band with 152 px black bars top and bottom, HUD bunched in the middle). Right: modded (3D to every edge, AP/EN bottom-left, ammo bottom-right, compass bottom-centre, round reticle). Photograph the tube itself, not a capture. -->

<!-- MEDIA: side-by-side photo of the garage on the CRT. Left: vanilla ASSEMBLY or garage top menu inside the 16:9 band. Right: modded (title near the top edge, list under the title strip, hint bar on the bottom edge, no band visible). -->

<!-- MEDIA (optional): a short GIF of the agent loop: a menu.py nudge, the relaunch, then the capture.sh PNG showing the element in its new place. -->

## What it does

- Fills the whole 1280x1024 resolution in missions, the garage and cutscenes: no letterbox, no stretch, round reticle, dynamic resolution still working.
- Removes the UI letterbox frame bars.
- Moves the HUD gauges (AP, EN, ammo, radar, boss bar) out to the 5:4 edges. The reticle and lock-on markers are untouched, so they stay where the engine puts them.
- Reflows the garage menus (top menu, Assembly, Paint, Decal, Image Editor, AC Data, OS Tuning, Arena, System, Missions, Parts Shop and more) so the old 16:9 band isn't visible.
- A rofi launcher with two modes. **CRT** puts the mods in place, turns EAC off, sets exclusive fullscreen 1280x1024 and starts through me3. **Main** strips every mod, puts the genuine EAC launcher back, and refuses to launch online unless the game folder checks out as vanilla by md5.

## How it works

The game's picture comes from two separate systems, so the mod has two layers.

### Layer 1: the D3D12 hook (`hook/`)

`dxgi_vertplus.cpp` builds as a `dxgi.dll` proxy. It forwards every real export to `dxgi_proxy.dll`, a renamed copy of the prefix's DXVK dxgi (`dxgi.def`). At load it creates a throwaway D3D12 device and command list, reads their vtables, and uses MinHook to detour `RSSetViewports` and `RSSetScissorRects`, the draw, clear, pipeline and topology calls, resource and pipeline creation, and `Present`/`ExecuteCommandLists`.

- **The lever.** AC6 renders any aspect ratio cleanly (ultrawide works), but it clamps the narrow side to 16:9. On a 5:4 screen the scene goes into a `{0,152,1280,720}` viewport with explicit black-bar viewports above and below. The executable is Arxan-protected, so a static scan was useless. Instead an in-process scanner (`aspscan`) found the 16/9 and 9/16 float constants in the decrypted image, and `asppatch` rewrites all four to 1.0.
- **Timing.** The engine computes its render aspect once, during graphics init, before a D3D12 device exists to hook. Patching from the hook's init thread was already too late. So the patch thread is started from `DllMain` at process attach, ahead of the engine's first compute. No manual resolution change is needed.
- **The composite.** Even with the scene at 5:4, the final blit still landed in the 16:9 sub-rectangle. `autovp` expands that one composite viewport to the full swapchain, but only once a live 5:4 scene viewport has been seen for 8 frames. Doing the expansion during loading turns every texture black, and gating on the live scene avoids that.
- **The bars.** The leftover UI frame bars are drawn by one pipeline. It was found by automated bisection over the distinct PSO set: skip half, dump the backbuffer, and let `tools/check_bars.py --verdict` read the bar rows to say PRESENT or GONE. The pipeline index changes every launch (heap ASLR), so the hook fingerprints pipelines at creation by an FNV-1a hash of their shader bytecode and skips only that hash's 72-index draws. The button-prompt glyphs in the same pipeline are left alone.
- **What didn't work** (all kept as default-off switches in the source):
  - A plain viewport stretch fills the screen but makes the reticle oval.
  - Resizing the 16:9 render targets to 5:4 at creation gave real Vert+ in the garage but corrupted missions (depth and deferred passes no longer matched).
  - Scaling the projection in constant buffers did nothing visible: the camera is bound through descriptor tables. Scanning on every bind also made the game unplayably slow.
  - All the diagnostics (`capevery`, `aspscan`, `pfval`/`pfheap`, draw and PSO dumps) can be toggled live from `Game/ac6ctl.txt`, which the hook re-reads every 30 frames.

### Layer 2: the Scaleform UI (`ui/`)

The 3D fix didn't move a single menu, so the next step was proving where the UI's 16:9 layout actually comes from:

- Patching the 1920.0 and 1080.0 canvas constants moved only background art. Text stayed put.
- A reversible writable-heap scan replaced 2,084 in-memory copies of 16/9 with 5:4. The game survived, and the System menu still measured exactly y=152..864.
- Patching 9/16 (18 copies) crashed the game.

So no aspect value drives the UI. FromSoftware's menus and HUD are Scaleform `.gfx` (compiled Flash) with the 16:9 layout baked in. The fix is to edit the files: unpack them with UXM, convert them `.gfx` to XML and back with JPEXS FFDec, and load the edits with **me3**. ModEngine2 2.1.0 is too old for the current build; its signature scan never matched.

- **`hud.py`: the HUD.** The reticle, lock-on and target markers are placed every frame by the engine using 16:9 maths. The HUD's ActionScript has no positioning code for them, so they can't be moved and don't need to be. The trick that worked: keep the HUD stage at 16:9, so the engine's reticle maths stays centred, and move the *static* gauge containers to coordinates past the stage rectangle. Scaleform renders outside the stage without clipping. `hud.py` moves whole containers in screen pixels (1 px = 30 twips) and always rebuilds from a cached vanilla XML, so edits never compound.
- **`menu.py`: the menus.** This is the same pipeline for any menu file, with operations added as each wall turned up: `dump`, `nudge`/`set`, `deep` (move a nested `sprite:depth` placement), `explode`, `cover`, `opaque`, `vfill`/`deepvfill`, `clone`, a gated `stretch`, `build` and `reset`. Per-screen edits live in `ui/menu_offsets/*.json`.

**The walls in the garage, and how each was proven:**

1. **A persistent shell.** `01_garageterminal` is drawn under *every* garage sub-screen. In vanilla, each sub-screen's opaque band background hides the shell's title and hint bar. Moving the shell's elements to the true edges pushed them outside that cover, so they doubled up with the sub-screen's own title and hint bar. The file's timeline is only FadeIn/Loop/FadeOut with no per-screen state, so the shell can't be hidden, only covered. That forced a per-screen approach.
2. **381 clip masks.** The Assembly parts list (`02_010_equiptop`) has 381 clipDepth mask elements holding its content to the band. A stretch filled the content but still read wrong and couldn't cover the shell.
3. **Engine-drawn chrome.** The title-plate strip, the description bar, the COAM plate and the AC SPECS panel exist in none of the 98 cached `.gfx` files. Every file was scanned, including scale-aware bounds walks. Moving their apparent containers split them: the icons moved, but the plate and its text stayed where the engine put them. Both tests were reverted.

**The layout that shipped** treats the engine chrome as fixed anchors and works around it:

- `vfill` scales band-covering background layers by 1024/720 about the canvas centre, so the band disappears without stretching any content.
- Titles move to the top edge, and hint bars and their dark under-bars move to the bottom edge.
- Wide opaque lists pull up and cover the engine title strip. Narrow lists sit just under it, so the strip reads as a header rule.
- When the engine overwrites a root placement every frame (a nudge does nothing), its children get moved instead.

The game's own `.gfx` files are not in this repo. `menu_offsets/` and `hud_offsets.json` hold only the author's edit operations, which the tools apply to your own unpacked copies.

## Workflow for this project

The work was done in sessions with an AI coding agent driving the game on the real CRT. The tooling was built so the agent could check its own changes:

- **`ui/capture.sh` gives the agent eyes.** The compositor can't screenshot a fullscreen game (direct scanout returns black). So the hook copies the real backbuffer to a readback heap and dumps it to `ac6cap.bin`, `hook/tools/capraw2png.py` turns that into a PNG, and the agent reads the image. Every HUD and menu change was judged on the actual frame, not on numbers.
- **The verification loop.** Edit offsets, run `menu.py build`, relaunch through the launcher, and wait for a *fresh* me3 log with the expected override count. Then drive to the screen with `ydotool` key presses, capture, read the PNG, and nudge again. The agent captured before any risky key press because the cursor position can't be assumed.
- **A pixel oracle for bisection.** `check_bars.py --verdict` turned "are the bars gone?" into a one-word answer from the backbuffer. That let the agent binary-search the whole distinct-pipeline set for the bar PSO without a person watching.
- **Finding elements by pixels, not names.** The agent located garage elements by width, colour and computed on-screen Y, walking nested placements. The 98-file scan that proved the engine chrome isn't Scaleform was run the same way.
- **Telling regressions from runtime state.** When the garage came up pillarboxed, the agent confirmed by md5 that the shipped files were unchanged. The real cause was a degraded swapchain from repeated relaunches, and a reboot fixed it. "Verify the files first" became a standing rule.

## Requirements

- Linux with Steam and Proton, Armored Core VI (appid 1888160). Only tested this way.
- A 1280x1024 display set up for exclusive fullscreen.
- A mingw-w64 C++ cross compiler: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (used for the shipped build) or `mingw-w64-gcc`.
- Python 3, plus Pillow for `capraw2png.py`. Optional: `capstone` for `disasm_asp.py`, RenderDoc for `analyze_rdc.py`.
- For the UI layer:
  - Java and [JPEXS FFDec](https://github.com/jindrapetrik/jpexs-decompiler) in `ui/tools/ffdec/`
  - [me3](https://github.com/garyttierney/me3) (v0.11.0 used) in `ui/tools/me3/`
  - [UXM Selective Unpacker](https://github.com/Nordgaren/UXM-Selective-Unpack), a GUI run under Wine, to extract the vanilla menu `.gfx`
- For the launcher: Hyprland (the Lua-config build's `hl.dsp.*` dispatchers), rofi, gamescope (Main mode only).

## Install

```bash
git clone https://github.com/CipherNovaa/ac6-crt-5to4 && cd ac6-crt-5to4
cp config.example.env config.env   # edit paths / CRT output, then: set -a; . ./config.env; set +a

# 1. Build the hook
./hook/build.sh                    # -> hook/build/dxgi-ac6-crt.dll

# 2. One-time state the launcher needs (kept outside the game folder)
mkdir -p "$AC6_STATE_DIR"
cp "$AC6_PREFIX/drive_c/windows/system32/dxgi.dll" "$AC6_STATE_DIR/dxgi_proxy.dll"
cp "$AC6_GAME_DIR/start_protected_game.exe" "$AC6_STATE_DIR/start_protected_game.exe.genuine"   # while it is still genuine
md5sum "$AC6_STATE_DIR/start_protected_game.exe.genuine"   # put this in AC6_GENUINE_EAC_MD5

# 3. UI layer: unpack the vanilla menu .gfx with UXM into ui/gfx/vanilla/, then build what you want
./ui/hud.py build
for f in ui/menu_offsets/*.json; do ./ui/menu.py "$(basename "$f" .json)" build; done
```

## Usage

- `launcher/armored-core-6.sh` opens a rofi menu. **CRT** is offline and modded; **Main** is vanilla, online, and gated.
- Adjust the HUD: `./ui/hud.py show`, `./ui/hud.py nudge ammo 0 10`, then relaunch.
- Adjust a menu: `./ui/menu.py 02_osupdate dump` to list placements, `nudge <key> dx dy` or `deep <sprite> <depth> dx dy`, and `reset` to go back to vanilla instantly.
- See a frame: `./ui/capture.sh out.png` while the CRT mode is running. Afterwards set `capevery=0` in `Game/ac6ctl.txt`, because continuous dumps cost frame time.

**Never go online with the mod in place.** The edited files and the disabled EAC would get the account banned. me3 blocks online by default, and the launcher's Main mode removes every mod and checks the genuine EAC launcher's md5 before it starts the game.

## Repository layout

```
hook/          dxgi_vertplus.cpp, dxgi.def, build.sh
hook/tools/    capraw2png.py, check_bars.py, disasm_asp.py, analyze_rdc.py, test-headless.sh, vpad.py
hook/third_party/minhook/   MinHook (BSD-2-Clause), vendored
ui/            hud.py, menu.py, capture.sh, crt-me3-launch.sh, edit_bg_to_54.sh, ac6.me3.example
ui/menu_offsets/   per-screen edit operations (17 garage screens)
ui/legacy/     me2_wrapper.c (the abandoned ModEngine2 route)
launcher/      armored-core-6.sh, ac6-set-graphicsconfig.py, ac6-set-launchopts.py
```

`vpad.py` (a uinput virtual gamepad) and `test-headless.sh` (a virtual 5:4 output harness) are kept as records of dead ends: the game ignores hot-plugged pads and unfocused synthetic keys.

## Credits and licence

MIT, copyright (c) 2026 CipherNovaa. See `LICENSE`.

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, BSD-2-Clause, vendored with its licence.
- [me3](https://github.com/garyttierney/me3), [JPEXS FFDec](https://github.com/jindrapetrik/jpexs-decompiler) and [UXM Selective Unpacker](https://github.com/Nordgaren/UXM-Selective-Unpack) are used as external tools and are not included.
- An Elden Ring 16:10 UI fix (pure `.gfx` edits) was studied as a reference for the technique. None of its files are included.

Armored Core VI is a trademark of FromSoftware / Bandai Namco. No game files are included or distributed.
