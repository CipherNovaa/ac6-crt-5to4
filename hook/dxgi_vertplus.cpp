// AC6 Vert+ hook — Phase 1 (logger) + Phase 2 (viewport rewrite, behind ENABLE_REWRITE).
//
// Builds as a dxgi.dll proxy (see dxgi.def). At load it creates a dummy D3D12
// device + command list to grab the ID3D12GraphicsCommandList vtable, then
// MinHook-detours RSSetViewports / RSSetScissorRects. Phase 1 just logs every
// call to Game/ac6vp.log so we can confirm the engine sets a 16:9 letterbox
// viewport ({0,152,1280,720}) for the 3D scene pass. Once confirmed, define
// ENABLE_REWRITE to expand matching viewports to fill 1280x1024.
//
// Vtable indices (ID3D12GraphicsCommandListVtbl, d3d12.h):
//   ... QueryInterface=0 AddRef=1 Release=2 ... Close=9 Reset=10 ...
//   RSSetViewports=21  RSSetScissorRects=22   (VERIFY at build time)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <set>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include "MinHook.h"

#define VP_INDEX 21
#define SR_INDEX 22

static FILE* g_log = nullptr;
static HMODULE g_self = nullptr;   // our own module handle (set in DllMain)
static DWORD WINAPI aspect_scan_thread(LPVOID);   // fwd decl (used by hkPresent live-trigger)
static void LG(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---- targets ----
typedef void (STDMETHODCALLTYPE *RSSetViewports_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef void (STDMETHODCALLTYPE *RSSetScissorRects_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
typedef void (STDMETHODCALLTYPE *SetGfxRoot32_t)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
typedef void (STDMETHODCALLTYPE *SetGfxRootCBV_t)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
static RSSetViewports_t    oRSSetViewports    = nullptr;
static RSSetScissorRects_t oRSSetScissorRects = nullptr;
static SetGfxRoot32_t      oSetGfxRoot32      = nullptr;
static SetGfxRootCBV_t     oSetGfxRootCBV     = nullptr;

// ---- letterbox-hunt targets (Phase 3: draw-call RE for the 2D UI bars) ----
typedef void (STDMETHODCALLTYPE *DrawInstanced_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *DrawIndexedInstanced_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *IASetPrimitiveTopology_t)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
typedef void (STDMETHODCALLTYPE *SetPipelineState_t)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void (STDMETHODCALLTYPE *ClearRTV_t)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
static DrawInstanced_t          oDrawInstanced          = nullptr;
static DrawIndexedInstanced_t   oDrawIndexedInstanced   = nullptr;
static IASetPrimitiveTopology_t oIASetPrimitiveTopology = nullptr;
static SetPipelineState_t       oSetPipelineState       = nullptr;
static ClearRTV_t               oClearRTV               = nullptr;

// vtable indices (ID3D12GraphicsCommandList, verified: RSSetViewports=21 worked)
#define IDX_DrawInstanced                 12
#define IDX_DrawIndexedInstanced          13
#define IDX_IASetPrimitiveTopology        20
#define IDX_RSSetViewports                21
#define IDX_RSSetScissorRects             22
#define IDX_SetPipelineState              25
#define IDX_SetGraphicsRoot32BitConstants 36
#define IDX_SetGraphicsRootConstantBufferView 38
#define IDX_ClearRenderTargetView         50

// 16:9 projection signature: a 4x4 (row-major) matrix where m00/m11 == 0.5625
// (=9/16) with the perspective zero-pattern. To make it 5:4 (ratio 0.8) we scale
// m11 (vertical FOV term) by 0.703125 (=0.5625/0.8) -> widens vertical view = Vert+.
static const float ASPECT_RATIO_16_9 = 0.5625f;     // m00/m11 for 16:9
static const float M11_SCALE = 0.5625f / 0.8f;      // 0.703125

static const float FB_W = 1280.0f, FB_H = 1024.0f;
static const float CONTENT_H = 720.0f;   // 1280*9/16
static const float BAR = 152.0f;         // (1024-720)/2

static bool is_letterbox_vp(const D3D12_VIEWPORT& v) {
    // 16:9 content centered in the 5:4 buffer
    return fabsf(v.Width - FB_W) < 2.0f && fabsf(v.Height - CONTENT_H) < 2.0f
        && fabsf(v.TopLeftY - BAR) < 2.0f;
}

static std::set<uint64_t> g_seenVP, g_seenSR;
static uint64_t key4(int a,int b,int c,int d){
    return ((uint64_t)(a&0xFFFF)<<48)|((uint64_t)(b&0xFFFF)<<32)|((uint64_t)(c&0xFFFF)<<16)|(uint64_t)(d&0xFFFF);
}

// ---- live-tunable Vert+ controls (read from Game/ac6ctl.txt) ----
static float g_cy   = 0.703125f;  // clip-Y scale (=720/1024). <1 widens vertical FOV
static int   g_vpfix = 0;         // viewport rewrite — OFF (fill comes from asppatch+autovp; vpfix=1 stretches) [PROD]
static int   g_pscale = 0;        // projection clip-Y scaling — OFF (not needed with asppatch) [PROD]
static int   g_logmat = 0;        // log matrices found
static int   g_capEvery = 0;      // frame-capture cadence (0 = off; set via control file)
static int   g_rtresize = 0;      // resize 16:9 render targets to 5:4 at creation (true Vert+)
static float g_tarmm = 0.8f;      // projection target m00/m11 (0.8 = exact 5:4); tune to round the reticle
static int   g_aspscan = 0;       // run the aspect-clamp hunter once at startup
static int   g_uiscan = 0;        // include UI-canvas dims (1920/1080) in the aspect scan
// ---- reversible by-value float patch (UI-canvas lever hunt) ----
// pfval = float value to find in the image data, pfnew = value to replace it with.
// Applied live on change (hkPresent re-reads ctl every 30 frames); pfval=0 restores.
// Lets me test canvas/aspect candidates against the title screen WITHOUT relaunching.
static float g_pfval = 0.0f;
static float g_pfnew = 0.0f;
static int   g_pfheap = 0;        // also scan writable heap (MEM_PRIVATE) for pfval, not just the module image
static std::vector<std::pair<unsigned*,unsigned>> g_pfRecord;
static float g_pfvalApplied = 0.0f, g_pfnewApplied = 0.0f;
static void apply_pf_patch();   // fwd (defined near patch_aspect_now)
static int   g_asppatch = 3;      // 1=patch 16/9 consts, 2=patch 9/16, 3=both (->1.0)  [PROD default ON]
static int   g_autovp = 1;        // auto-gate vpfix: only expand composite when scene is live 5:4  [PROD default ON]
static int   g_garagefill = 0;    // expand the garage's 16:10 {0,112,1280,800} composite to 5:4 (experimental: may stretch)
static volatile long g_frame = 0; // present counter (declared early; used by autovp gate)
static volatile long g_last5f = -100000; // last frame a large 5:4 scene viewport was seen
static int   g_cutdump = 0;       // capture: log ALL large (w>=1000) viewports every 10th frame
                                  // (race-free, no dedup) so cutscene/menu viewports are visible

// ---- letterbox-hunt state (Phase 3) ----
// We track the most-recent per-command-list render state globally. The direct queue
// records serially, so the last viewport/topology/PSO set before a draw is that draw's
// state in practice. Good enough to FINGERPRINT the letterbox bar draw.
static thread_local D3D12_VIEWPORT g_curVP = {0,0,0,0,0,0};
static thread_local D3D12_RECT     g_curSR = {0,0,0,0};
static thread_local UINT           g_curTopo = 0;
static thread_local void*          g_curPSO  = nullptr;
static int      g_drawdump = 0;   // log draws/clears whose state looks letterbox-ish (sampled)
static int      g_clrdump  = 0;   // log ALL ClearRenderTargetView calls (color + rects), sampled
// live-tunable SKIP rules (set once the bar signature is known; no relaunch needed):
static int      g_skipvc   = 0;   // skip DrawInstanced w/ this exact VertexCountPerInstance (0=off) when fullscreen
static int      g_skipivc  = 0;   // skip DrawIndexedInstanced w/ this exact IndexCountPerInstance (0=off) when fullscreen
static int      g_clrbar   = 0;   // suppress black ClearRenderTargetView calls that carry partial rects (the bars)
// ---- draw-call bisection (find the exact bar draw by per-frame ordinal) ----
static volatile long g_drawOrd = 0;     // per-frame draw ordinal (reset each Present)
static volatile long g_lastDrawN = 0;   // total draws in the previous frame (for range planning)
static int      g_skipord  = 0;   // enable ordinal-range skip
static int      g_ordlo    = 0;   // skip draws with ordinal in [ordlo, ordhi)
static int      g_ordhi    = 0;
// ---- PSO-set bisection (thread-stable: skip draws by frozen PSO-list index) ----
static std::set<void*>    g_psoSeen;       // every distinct PSO seen (live)
static std::vector<void*> g_psoFrozen;     // sorted snapshot to bisect over
static CRITICAL_SECTION   g_psoCs;
static int      g_psofreeze = 0;  // rising edge -> snapshot g_psoSeen into g_psoFrozen
static int      g_psklo    = -1;  // skip draws whose PSO sorted-index in [psklo, pskhi)
static int      g_pskhi    = -1;
static unsigned g_skippso  = 0;   // skip draws whose (PSO low-32-bits) == this (final pin)
static int      g_pindump  = 0;   // log vc/ic/vp/scissor of draws whose PSO is in the skip range
static int      g_pinwide  = 0;   // when PSO-skipping, only skip WIDE draws (scissor w>=pinwmin) = the bars, spare arrows
static int      g_pinwmin  = 1000;// width threshold for "wide" (the full-width bars)
static int      g_pinic    = 0;   // when PSO-skipping, only skip draws whose index/vertex count == this (0=all)
// ---- launch-stable bar-PSO identification by shader hash (production) ----
static std::unordered_map<void*, uint64_t> g_psoHash;   // PSO ptr -> VS^PS bytecode hash
static std::set<void*> g_barPSOs;                       // PSO ptrs whose hash == g_barhash
static uint64_t g_barhash = 0x2acc438b88cb8685ULL; // bar-PSO shader hash (FNV-1a VS^PS), found via PSO bisection [PROD]
static int      g_baric   = 72;  // index count of the BAR draws within that PSO (ic=72; ic=30/21 = hover arrows, kept) [PROD]
static int      g_pinhash = 0;   // diag: log frozen[psklo] ptr + shader hash each reload
static thread_local bool g_curIsBar = false;            // does g_curPSO match the bar hash?
// ---- fade-box overlay kill (the transient full-screen black panel on garage entry) ----
static int      g_killfs4 = 0;   // skip FULLSCREEN-viewport vc=4 quads (the alpha-fade overlays) [gated=safe]
static uint64_t g_boxhash = 0;   // optional precise target: fade-overlay PSO shader hash
static std::set<void*> g_boxPSOs;
static thread_local bool g_curIsBox = false;
// ---- drawn-PSO diff (non-breaking box identification) ----
static std::set<void*> g_drawnPSO;     // PSOs actually drawn since last snapshot
static std::set<void*> g_drawnSteady;  // snapshot taken in steady state
static int g_drawacc  = 0;  // accumulate drawn PSOs
static int g_drawsnap = 0;  // rising edge: steady = drawn; clear drawn
static int g_drawdiff = 0;  // rising edge: log (drawn - steady) with ptr/hash/frozen-index
static unsigned g_skipptr = 0; // skip draws whose PSO low-32 == this (single-PSO confirm, non-breaking)
// bisect the transient set (drawn-but-not-steady) by PSO low-32 range — gated so only
// transient fullscreen-quad PSOs are touched (won't break steady UI/scene).
static int      g_skiptrans = 0;
static int      g_steadymode = 0;   // 1 = gate on STEADY membership instead of transient
static unsigned g_tlo = 0, g_thi = 0;

static void STDMETHODCALLTYPE hkRSSetViewports(ID3D12GraphicsCommandList* self, UINT n, const D3D12_VIEWPORT* vps) {
    if (vps && n) g_curVP = vps[0];   // track for the letterbox-hunt draw fingerprint
    if (vps && n) {
        // log only DISTINCT viewports (survives to gameplay; no per-frame spam)
        uint64_t k = key4((int)lroundf(vps[0].TopLeftX),(int)lroundf(vps[0].TopLeftY),
                          (int)lroundf(vps[0].Width),(int)lroundf(vps[0].Height));
        if (g_seenVP.insert(k).second)
            LG("VP n=%u [0] x=%.1f y=%.1f w=%.1f h=%.1f d=%.2f..%.2f",
               n, vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height,
               vps[0].MinDepth, vps[0].MaxDepth);
    }
    // cutscene/menu capture: dump full-width viewports (incl. thin bar quads), sampled
    if (g_cutdump && vps && n && (g_frame % 10) == 0) {
        for (UINT i = 0; i < n; i++) {
            if (vps[i].Width >= 1000.0f && vps[i].Height >= 1.0f)
                LG("CUTVP fr=%ld n=%u [%u] x=%.0f y=%.0f w=%.0f h=%.0f ar=%.3f", g_frame, n, i,
                   vps[i].TopLeftX, vps[i].TopLeftY, vps[i].Width, vps[i].Height,
                   vps[i].Width / vps[i].Height);
        }
    }
    // autovp: detect a large, LIVE 5:4 *scene* viewport (the dynamic-resolution render
    // pass: 1278x1022, 1266x1010, ... -> always width < 1280 and ratio ~1.25). Its
    // presence means the engine is currently rendering the SCENE at 5:4 AND the mission
    // is loaded -> safe to expand the composite this frame. CRITICAL: must EXCLUDE the
    // full 1280x1024 (5:4) UI/post passes that exist in EVERY state (menu/garage) -- those
    // are exactly 1280 wide at (0,0); matching them would expand the garage composite ->
    // black. The garage scene stays 16:9 (1278x718) so it never trips this. During
    // loads/menus the dynamic-res 5:4 pass stops appearing, so the gate auto-closes
    // (no load-black footgun).
    if (g_autovp && vps && n) {
        for (UINT i = 0; i < n; i++) {
            float ww = vps[i].Width, hh = vps[i].Height;
            if (hh < 1.0f) continue;
            float ar = ww / hh;
            if (ww >= 1000.0f && ww < 1280.0f && ar > 1.20f && ar < 1.30f) { g_last5f = g_frame; break; }
        }
    }
    bool autovp_ok = g_autovp && ((g_frame - g_last5f) <= 8);
    if ((g_vpfix || g_autovp || g_garagefill || g_rtresize) && vps && n && n <= 16) {
        D3D12_VIEWPORT fixed[16];
        bool changed = false;
        for (UINT i = 0; i < n; i++) {
            fixed[i] = vps[i];
            float ww = vps[i].Width, hh = vps[i].Height;
            if (hh < 1.0f || ww < 1.0f) continue;
            float ar = ww / hh;
            // SURGICAL composite signature: the mission letterbox blit is FULL-WIDTH
            // (~1280, == swapchain width) AND offset DOWN by ~152 (TopLeftY>100). The
            // garage/UI 16:9 passes are narrower (dynamic-res, <1280) or at TopLeftY~0/1,
            // so this tight test never touches them (was the garage-leak bug: loose
            // ww>=16/TopLeftY>1 grabbed garage passes). rtresize mode keeps the broad expand.
            bool is_composite = (ww >= 1270.0f && vps[i].TopLeftY > 100.0f);
            // garage 16:10 composite {0,112,1280,800}: full-width, offset ~112, ar ~1.6
            bool is_garage = g_garagefill && (ww >= 1270.0f && ar > 1.55f && ar < 1.66f && vps[i].TopLeftY > 80.0f);
            // composite-EXPAND needs the freshness gate (autovp_ok) / manual vpfix / rtresize
            // so it never fires during loads (the load-black footgun).
            bool expand_ok = autovp_ok || g_vpfix || g_rtresize;
            // (a) the composite letterbox blit -> expand to 5:4 height, anchored at top.
            if ((expand_ok && ar > 1.70f && ar < 1.85f && (is_composite || (g_rtresize && ww >= 16.0f))) || is_garage) {
                fixed[i].Height   = ww / 1.25f;   // -> 5:4 (full-width 1280 -> 1024)
                fixed[i].TopLeftY = 0.0f;
                changed = true;
            }
            // (b) the full-width thin letterbox BAR quads (mission {..,152}; garage {..,112})
            //     -> collapse. Unconditional (NOT freshness-gated): these are never legit
            //     content, so killing them is always safe and removes residual bars left
            //     after the 3D fills. Full-width only, so garage UI strips are left alone.
            else if (ar > 4.0f && ww >= 1270.0f && hh < 200.0f) {
                fixed[i].Width = 0.0f; fixed[i].Height = 0.0f;  // degenerate: nothing rasterizes
                changed = true;
            }
        }
        if (changed) { oRSSetViewports(self, n, fixed); return; }
    }
    oRSSetViewports(self, n, vps);
}

static void STDMETHODCALLTYPE hkRSSetScissorRects(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RECT* rs) {
    if (rs && n) g_curSR = rs[0];   // track for PSO-180 bar/arrow discrimination
    if (rs && n) {
        uint64_t k = key4((int)rs[0].left,(int)rs[0].top,(int)rs[0].right,(int)rs[0].bottom);
        if (g_seenSR.insert(k).second)
            LG("SR n=%u [0] l=%ld t=%ld r=%ld b=%ld", n, (long)rs[0].left, (long)rs[0].top, (long)rs[0].right, (long)rs[0].bottom);
    }
    // cutscene/menu capture: dump full-width scissor rects (incl. thin bar strips)
    if (g_cutdump && rs && n && (g_frame % 10) == 0) {
        for (UINT i = 0; i < n; i++) {
            long w = rs[i].right - rs[i].left, h = rs[i].bottom - rs[i].top;
            if (w >= 1000)
                LG("CUTSR fr=%ld n=%u [%u] l=%ld t=%ld r=%ld b=%ld w=%ld h=%ld", g_frame, n, i,
                   (long)rs[i].left, (long)rs[i].top, (long)rs[i].right, (long)rs[i].bottom, w, h);
        }
    }
    bool autovp_ok = g_autovp && ((g_frame - g_last5f) <= 8);
    if ((g_vpfix || autovp_ok || g_garagefill) && rs && n && n <= 16) {
        D3D12_RECT fixed[16]; bool changed = false;
        for (UINT i = 0; i < n; i++) {
            fixed[i] = rs[i];
            long w = rs[i].right - rs[i].left, h = rs[i].bottom - rs[i].top;
            if (h > 1 && w >= 16) {
                float ar = (float)w/(float)h;
                // surgical: only the full-width composite letterbox scissor (top>100, w~1280);
                // rtresize: all 16:9. Tight test leaves garage/UI scissors untouched.
                bool is_composite = (w >= 1270 && rs[i].top > 100);
                // garage 16:10 scissor {0,112,1280,912}: full-width, top~112, ar~1.6
                bool is_garage = g_garagefill && (w >= 1270 && ar > 1.55f && ar < 1.66f && rs[i].top > 80);
                if ((ar > 1.70f && ar < 1.85f && (is_composite || g_rtresize)) || is_garage) {
                    fixed[i].top = 0; fixed[i].bottom = (long)(w / 1.25f); changed = true;
                }
            }
        }
        if (changed) { oRSSetScissorRects(self, n, fixed); return; }
    }
    oRSSetScissorRects(self, n, rs);
}

// ---- letterbox-hunt hooks (Phase 3) ----------------------------------------
// A "fullscreen-ish" current viewport means a draw is a candidate compositing /
// letterbox draw (covers ~the whole 1280x1024 backbuffer).
static inline bool curvp_fullscreen() {
    return g_curVP.Width >= 1200.0f && g_curVP.Height >= 960.0f;
}

static void STDMETHODCALLTYPE hkIASetPrimitiveTopology(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY t) {
    g_curTopo = (UINT)t;
    oIASetPrimitiveTopology(self, t);
}

static void STDMETHODCALLTYPE hkSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* ps) {
    g_curPSO = (void*)ps;
    if (ps) {
        EnterCriticalSection(&g_psoCs);
        g_psoSeen.insert((void*)ps);
        g_curIsBar = (!g_barPSOs.empty() && g_barPSOs.count((void*)ps) > 0);
        g_curIsBox = (!g_boxPSOs.empty() && g_boxPSOs.count((void*)ps) > 0);
        LeaveCriticalSection(&g_psoCs);
    } else { g_curIsBar = false; g_curIsBox = false; }
    oSetPipelineState(self, ps);
}

// thread-stable skip test: by frozen-PSO-list index range, or by pinned PSO low-32.
static inline bool pso_skipped(void* pso) {
    if (g_skippso && (unsigned)(uintptr_t)pso == g_skippso) return true;
    if (g_psklo >= 0 && g_pskhi > g_psklo && !g_psoFrozen.empty()) {
        auto it = std::lower_bound(g_psoFrozen.begin(), g_psoFrozen.end(), pso);
        if (it != g_psoFrozen.end() && *it == pso) {
            long idx = (long)(it - g_psoFrozen.begin());
            if (idx >= g_psklo && idx < g_pskhi) return true;
        }
    }
    return false;
}

// transient-set bisection: skip a draw only if its PSO is in the (drawn - steady) set
// AND its low-32 pointer falls in [g_tlo,g_thi). Gated so steady UI/scene is untouched.
static inline bool trans_skipped(void* pso) {
    if (!g_skiptrans) return false;
    unsigned lo32 = (unsigned)(uintptr_t)pso;
    if (lo32 < g_tlo || lo32 >= g_thi) return false;
    EnterCriticalSection(&g_psoCs);
    bool t = g_steadymode ? (g_drawnSteady.count(pso) > 0)
                          : (g_drawnPSO.count(pso) && !g_drawnSteady.count(pso));
    LeaveCriticalSection(&g_psoCs);
    return t;
}

static void STDMETHODCALLTYPE hkDrawInstanced(ID3D12GraphicsCommandList* self, UINT vc, UINT ic, UINT sv, UINT si) {
    long ord = InterlockedIncrement(&g_drawOrd) - 1;   // per-frame ordinal (bisection)
    if (g_drawacc && curvp_fullscreen() && vc <= 6) { EnterCriticalSection(&g_psoCs); g_drawnPSO.insert(g_curPSO); LeaveCriticalSection(&g_psoCs); }
    if (g_skipptr && (unsigned)(uintptr_t)g_curPSO == g_skipptr) return;  // single-PSO confirm
    if (trans_skipped(g_curPSO)) return;                                  // transient-set bisection
    if (g_skipord && ord >= g_ordlo && ord < g_ordhi) return;
    if (pso_skipped(g_curPSO)) {                        // PSO-set bisection / pin
        long srw = g_curSR.right - g_curSR.left;
        if (g_pindump && (g_frame % 10) == 0)
            LG("PIN D vc=%u ic=%u topo=%u vp=%.0fx%.0f sr=%ld,%ld,%ld,%ld srw=%ld",
               vc, ic, g_curTopo, g_curVP.Width, g_curVP.Height,
               (long)g_curSR.left,(long)g_curSR.top,(long)g_curSR.right,(long)g_curSR.bottom, srw);
        if ((!g_pinwide || srw >= g_pinwmin) && (g_pinic == 0 || (int)vc == g_pinic)) return;
    }
    // PRODUCTION: auto-detected bar PSO (by shader hash) + optional index-count filter
    if (g_curIsBar && (g_baric == 0 || (int)vc == g_baric)) return;
    // fade-box overlay: drop the full-screen alpha quads (gated to fullscreen = safe)
    if (g_killfs4 && vc == 4 && curvp_fullscreen()) return;
    if (g_curIsBox) return;
    // skip rule: once the bar draw's vertex count is known, drop it (ungated now)
    if (g_skipvc && (int)vc == g_skipvc) return;
    if (g_drawdump && (g_frame % 10) == 0 && curvp_fullscreen() && vc <= 64) {
        uint64_t h = 0; { EnterCriticalSection(&g_psoCs); if (g_psoHash.count(g_curPSO)) h = g_psoHash[g_curPSO]; LeaveCriticalSection(&g_psoCs); }
        LG("DRAW fr=%ld vc=%u ic=%u topo=%u pso=%p hash=0x%llx vp=%.0fx%.0f@%.0f,%.0f",
           g_frame, vc, ic, g_curTopo, g_curPSO, (unsigned long long)h,
           g_curVP.Width, g_curVP.Height, g_curVP.TopLeftX, g_curVP.TopLeftY);
    }
    oDrawInstanced(self, vc, ic, sv, si);
}

static void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D12GraphicsCommandList* self, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    long ord = InterlockedIncrement(&g_drawOrd) - 1;   // per-frame ordinal (bisection)
    if (g_drawacc && curvp_fullscreen() && ic <= 6) { EnterCriticalSection(&g_psoCs); g_drawnPSO.insert(g_curPSO); LeaveCriticalSection(&g_psoCs); }
    if (g_skipptr && (unsigned)(uintptr_t)g_curPSO == g_skipptr) return;  // single-PSO confirm
    if (trans_skipped(g_curPSO)) return;                                  // transient-set bisection
    if (g_skipord && ord >= g_ordlo && ord < g_ordhi) return;
    if (pso_skipped(g_curPSO)) {                        // PSO-set bisection / pin
        long srw = g_curSR.right - g_curSR.left;
        if (g_pindump && (g_frame % 10) == 0)
            LG("PIN DI ic=%u inst=%u topo=%u vp=%.0fx%.0f sr=%ld,%ld,%ld,%ld srw=%ld",
               ic, inst, g_curTopo, g_curVP.Width, g_curVP.Height,
               (long)g_curSR.left,(long)g_curSR.top,(long)g_curSR.right,(long)g_curSR.bottom, srw);
        if ((!g_pinwide || srw >= g_pinwmin) && (g_pinic == 0 || (int)ic == g_pinic)) return;
    }
    // PRODUCTION: auto-detected bar PSO (by shader hash) + optional index-count filter
    if (g_curIsBar && (g_baric == 0 || (int)ic == g_baric)) return;
    if (g_skipivc && (int)ic == g_skipivc) return;     // ungated now
    if (g_drawdump && (g_frame % 10) == 0 && curvp_fullscreen() && ic <= 64) {
        LG("DRAWI fr=%ld ic=%u inst=%u topo=%u pso=%p vp=%.0fx%.0f@%.0f,%.0f",
           g_frame, ic, inst, g_curTopo, g_curPSO,
           g_curVP.Width, g_curVP.Height, g_curVP.TopLeftX, g_curVP.TopLeftY);
    }
    oDrawIndexedInstanced(self, ic, inst, si, bv, sinst);
}

static void STDMETHODCALLTYPE hkClearRTV(ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE h,
                                         const FLOAT col[4], UINT nr, const D3D12_RECT* rs) {
    bool black = col && col[0] < 0.02f && col[1] < 0.02f && col[2] < 0.02f;
    // suppress: black partial-rect clears are the prime letterbox-bar suspect (cutdump
    // never saw these — ClearRTV rects are NOT RSSetScissorRects). Drop them entirely.
    if (g_clrbar && black && nr > 0) {
        if (g_clrdump) LG("CLRBAR-SKIP fr=%ld nr=%u rect0=%ld,%ld,%ld,%ld", g_frame, nr,
                          (long)rs[0].left,(long)rs[0].top,(long)rs[0].right,(long)rs[0].bottom);
        return;
    }
    if (g_clrdump && (g_frame % 10) == 0) {
        if (nr > 0) {
            for (UINT i = 0; i < nr && i < 8; i++)
                LG("CLR fr=%ld col=%.2f,%.2f,%.2f,%.2f nr=%u [%u] %ld,%ld,%ld,%ld", g_frame,
                   col?col[0]:-1,col?col[1]:-1,col?col[2]:-1,col?col[3]:-1, nr, i,
                   (long)rs[i].left,(long)rs[i].top,(long)rs[i].right,(long)rs[i].bottom);
        } else {
            LG("CLR fr=%ld col=%.2f,%.2f,%.2f,%.2f nr=0 (full)", g_frame,
               col?col[0]:-1,col?col[1]:-1,col?col[2]:-1,col?col[3]:-1);
        }
    }
    oClearRTV(self, h, col, nr, rs);
}

// Scan a float array for a perspective projection matrix (m00/m11==0.5625 with the
// perspective zero-pattern). Returns the index of m00, or -1. If patch && found,
// rescales m11 in place (Vert+).
static int find_proj(float* f, UINT n, bool patch) {
    if (n < 16) return -1;
    for (UINT i = 0; i + 16 <= n; i++) {
        float m00 = f[i], m11 = f[i+5];
        if (m00 == 0.0f || m11 == 0.0f) continue;
        // perspective zero pattern: m01,m02,m03,m10,m12,m13 == 0
        if (f[i+1]||f[i+2]||f[i+3]||f[i+4]||f[i+6]||f[i+7]) continue;
        float r = m00 / m11;
        if (fabsf(r - ASPECT_RATIO_16_9) < 0.01f) {
            if (patch) f[i+5] = m11 * M11_SCALE;
            return (int)i;
        }
    }
    return -1;
}

static void load_ctl() {
    FILE* f = fopen("ac6ctl.txt", "r"); if (!f) return;
    char ln[160]; float v;
    while (fgets(ln, sizeof(ln), f)) {
        if      (sscanf(ln, "cy=%f", &v) == 1)     g_cy = v;
        else if (sscanf(ln, "vpfix=%f", &v) == 1)  g_vpfix = (int)v;
        else if (sscanf(ln, "pscale=%f", &v) == 1) g_pscale = (int)v;
        else if (sscanf(ln, "logmat=%f", &v) == 1) g_logmat = (int)v;
        else if (sscanf(ln, "capevery=%f", &v) == 1) g_capEvery = (int)v;
        else if (sscanf(ln, "rtresize=%f", &v) == 1) g_rtresize = (int)v;
        else if (sscanf(ln, "tarmm=%f", &v) == 1) g_tarmm = v;
        else if (sscanf(ln, "aspscan=%f", &v) == 1) g_aspscan = (int)v;
        else if (sscanf(ln, "uiscan=%f", &v) == 1) g_uiscan = (int)v;
        else if (sscanf(ln, "pfval=%f", &v) == 1) g_pfval = v;
        else if (sscanf(ln, "pfnew=%f", &v) == 1) g_pfnew = v;
        else if (sscanf(ln, "pfheap=%f", &v) == 1) g_pfheap = (int)v;
        else if (sscanf(ln, "asppatch=%f", &v) == 1) g_asppatch = (int)v;
        else if (sscanf(ln, "autovp=%f", &v) == 1) g_autovp = (int)v;
        else if (sscanf(ln, "garagefill=%f", &v) == 1) g_garagefill = (int)v;
        else if (sscanf(ln, "cutdump=%f", &v) == 1) g_cutdump = (int)v;
        else if (sscanf(ln, "drawdump=%f", &v) == 1) g_drawdump = (int)v;
        else if (sscanf(ln, "clrdump=%f", &v) == 1) g_clrdump = (int)v;
        else if (sscanf(ln, "skipvc=%f", &v) == 1) g_skipvc = (int)v;
        else if (sscanf(ln, "skipivc=%f", &v) == 1) g_skipivc = (int)v;
        else if (sscanf(ln, "clrbar=%f", &v) == 1) g_clrbar = (int)v;
        else if (sscanf(ln, "skipord=%f", &v) == 1) g_skipord = (int)v;
        else if (sscanf(ln, "ordlo=%f", &v) == 1) g_ordlo = (int)v;
        else if (sscanf(ln, "ordhi=%f", &v) == 1) g_ordhi = (int)v;
        else if (sscanf(ln, "psofreeze=%f", &v) == 1) g_psofreeze = (int)v;
        else if (sscanf(ln, "psklo=%f", &v) == 1) g_psklo = (int)v;
        else if (sscanf(ln, "pskhi=%f", &v) == 1) g_pskhi = (int)v;
        else if (sscanf(ln, "skippso=%x", &g_skippso) == 1) ;
        else if (sscanf(ln, "pindump=%f", &v) == 1) g_pindump = (int)v;
        else if (sscanf(ln, "pinwide=%f", &v) == 1) g_pinwide = (int)v;
        else if (sscanf(ln, "pinwmin=%f", &v) == 1) g_pinwmin = (int)v;
        else if (sscanf(ln, "pinic=%f", &v) == 1) g_pinic = (int)v;
        else if (sscanf(ln, "pinhash=%f", &v) == 1) g_pinhash = (int)v;
        else if (sscanf(ln, "baric=%f", &v) == 1) g_baric = (int)v;
        else if (sscanf(ln, "barhash=%llx", &g_barhash) == 1) ;
        else if (sscanf(ln, "killfs4=%f", &v) == 1) g_killfs4 = (int)v;
        else if (sscanf(ln, "boxhash=%llx", &g_boxhash) == 1) ;
        else if (sscanf(ln, "drawacc=%f", &v) == 1) g_drawacc = (int)v;
        else if (sscanf(ln, "drawsnap=%f", &v) == 1) g_drawsnap = (int)v;
        else if (sscanf(ln, "drawdiff=%f", &v) == 1) g_drawdiff = (int)v;
        else if (sscanf(ln, "skipptr=%x", &g_skipptr) == 1) ;
        else if (sscanf(ln, "skiptrans=%f", &v) == 1) g_skiptrans = (int)v;
        else if (sscanf(ln, "steadymode=%f", &v) == 1) g_steadymode = (int)v;
        else if (sscanf(ln, "tlo=%x", &g_tlo) == 1) ;
        else if (sscanf(ln, "thi=%x", &g_thi) == 1) ;
    }
    fclose(f);
}

// Scale clip-Y of perspective transforms (works on pure proj AND combined viewProj):
// indices {1,5,9,13} are the clip.y contributors in both row- and column-major.
// Perspective marker: m33(idx15)~0 and a w-from-z term (idx11 or idx14 ~ +/-1).
static CRITICAL_SECTION g_csf;            // protects g_scaledFrame
static std::set<uintptr_t> g_scaledFrame; // CBs scaled this frame (anti-compound)
// NORMALIZE the camera matrix to an exact target aspect (g_tarmm = m00/m11). With
// rtresize+vpfix the camera is already ~5:4 (ratio ~0.8) but slightly off (slight
// oval); we force it to exactly g_tarmm. IDEMPOTENT (re-applying scales by ~1.0),
// so no compounding and it's cheap even per-bind. i+=4 (matrices are float4-aligned).
static int scale_clipY_persp(float* f, UINT n) {
    int cnt = 0;
    for (UINT i = 0; i + 16 <= n; i += 4) {
        if (fabsf(f[i+15]) > 0.001f) continue;                 // affine/UI/identity -> skip
        float a = f[i+11], b = f[i+14];
        if (fabsf(fabsf(a)-1.0f) > 0.06f && fabsf(fabsf(b)-1.0f) > 0.06f) continue;
        float cx = sqrtf(f[i+0]*f[i+0]+f[i+4]*f[i+4]+f[i+8]*f[i+8]+f[i+12]*f[i+12]);
        float cy = sqrtf(f[i+1]*f[i+1]+f[i+5]*f[i+5]+f[i+9]*f[i+9]+f[i+13]*f[i+13]);
        if (cx < 1e-5f || cy < 1e-5f) continue;
        float ratio = cx / cy;
        // catch the camera whether it's 16:9 (~0.5625) OR already ~5:4 (~0.8); force to
        // EXACTLY g_tarmm. Window [0.50,0.90] excludes square (~1.0) shadow/UI matrices.
        // Idempotent: once at g_tarmm (0.8) it re-scales by ~1.0.
        if (ratio < 0.50f || ratio > 0.90f) continue;
        float s = ratio / g_tarmm;   // make clip-Y bigger/smaller so cx/cy -> g_tarmm
        f[i+1] *= s; f[i+5] *= s; f[i+9] *= s; f[i+13] *= s;
        cnt++;
    }
    return cnt;
}

static std::set<int> g_seen32, g_seenCBV;
static void STDMETHODCALLTYPE hkSetGfxRoot32(ID3D12GraphicsCommandList* self, UINT rootIdx, UINT num, const void* data, UINT destOff) {
    if (data && num >= 16 && num < 8192) {
        // copy so we can scan (and optionally patch) without touching caller memory
        static thread_local float buf[8192];
        memcpy(buf, data, num * 4);
        int at = find_proj(buf, num,
#ifdef ENABLE_REWRITE
            true
#else
            false
#endif
        );
        if (at >= 0) {
            if (g_seen32.insert(rootIdx).second)
                LG("PROJ in Root32BitConstants: root=%u num=%u at_float=%d (m00/m11=0.5625)", rootIdx, num, at);
#ifdef ENABLE_REWRITE
            oSetGfxRoot32(self, rootIdx, num, buf, destOff);
            return;
#endif
        }
    }
    oSetGfxRoot32(self, rootIdx, num, data, destOff);
}

// ---- upload-heap (CBV) tracking: GPU VA -> CPU pointer ----
struct MapRegion { void* res; unsigned long long va_base, size; uintptr_t cpu_base; };
static MapRegion g_regions[8192];
static volatile long g_nreg = 0;
static CRITICAL_SECTION g_cs;
static std::unordered_map<void*, long> g_resIdx;  // resource* -> g_regions index (O(1))
typedef HRESULT (STDMETHODCALLTYPE *Map_t)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
static Map_t oMap = nullptr;

static HRESULT STDMETHODCALLTYPE hkMap(ID3D12Resource* self, UINT sub, const D3D12_RANGE* rr, void** pp) {
    HRESULT hr = oMap(self, sub, rr, pp);
    if (SUCCEEDED(hr) && pp && *pp) {
        // fast path: O(1) hash lookup -> just refresh cpu_base (no GetDesc/GVA, no scan)
        EnterCriticalSection(&g_cs);
        auto it = g_resIdx.find(self);
        if (it != g_resIdx.end()) {
            g_regions[it->second].cpu_base = (uintptr_t)*pp;
            LeaveCriticalSection(&g_cs);
            return hr;
        }
        LeaveCriticalSection(&g_cs);
        // slow path (first time for this resource): only buffers
        D3D12_RESOURCE_DESC d = self->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            unsigned long long va = self->GetGPUVirtualAddress();
            if (va) {
                EnterCriticalSection(&g_cs);
                if (g_nreg < (long)(sizeof(g_regions)/sizeof(g_regions[0]))) {
                    long i = g_nreg++;
                    g_regions[i].res = self; g_regions[i].va_base = va; g_regions[i].size = d.Width; g_regions[i].cpu_base = (uintptr_t)*pp;
                    g_resIdx[self] = i;
                }
                LeaveCriticalSection(&g_cs);
            }
        }
    }
    return hr;
}

typedef void (STDMETHODCALLTYPE *Unmap_t)(ID3D12Resource*, UINT, const D3D12_RANGE*);
static Unmap_t oUnmap = nullptr;
static void STDMETHODCALLTYPE hkUnmap(ID3D12Resource* self, UINT sub, const D3D12_RANGE* wr) {
    // DXVK keeps UPLOAD heaps persistently mapped, so we DON'T invalidate cpu_base
    // (the pointer stays valid). O(1), no scan. (kept as a hook only for symmetry.)
    oUnmap(self, sub, wr);
}

static uintptr_t va_to_cpu(unsigned long long va, unsigned long long* avail) {
    uintptr_t cpu = 0;
    EnterCriticalSection(&g_cs);
    for (long i = 0; i < g_nreg; i++) {
        if (g_regions[i].cpu_base && g_regions[i].size && va >= g_regions[i].va_base && va < g_regions[i].va_base + g_regions[i].size) {
            unsigned long long off = va - g_regions[i].va_base;
            cpu = g_regions[i].cpu_base + off;
            if (avail) *avail = g_regions[i].size - off;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return cpu;
}

static void STDMETHODCALLTYPE hkSetGfxRootCBV(ID3D12GraphicsCommandList* self, UINT rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (g_pscale && rootIdx <= 2) {   // the camera viewProj lives in root 0/1
        unsigned long long avail = 0;
        uintptr_t cpu = va_to_cpu((unsigned long long)va, &avail);
        if (cpu) {
            UINT nf = (UINT)(avail / 4); if (nf > 1024) nf = 1024;
            int c = scale_clipY_persp((float*)cpu, nf);
            if (c && g_logmat && g_seenCBV.insert((int)rootIdx).second)
                LG("CBV root=%u scaled %d (16:9 cam) cy=%.4f", rootIdx, c, g_cy);
        }
    }
    oSetGfxRootCBV(self, rootIdx, va);
}

static void** g_res_vtbl = nullptr;
static void** g_dev_vtbl = nullptr;
#define IDX_Map 8   // ID3D12Resource::Map
#define IDX_Unmap 9 // ID3D12Resource::Unmap
#define IDX_CreateCommittedResource 27   // ID3D12Device::CreateCommittedResource
#define IDX_CreatePlacedResource 29      // ID3D12Device::CreatePlacedResource
#define IDX_CreateGraphicsPipelineState 10 // ID3D12Device::CreateGraphicsPipelineState

// ---- PSO shader-hash fingerprinting (launch-stable bar-PSO identification) ----
// We FNV-1a hash each PSO's VS+PS bytecode at creation and map pointer->hash. The
// letterbox PSO has the same shader hash every launch (its frozen index does NOT),
// so once we know that hash we auto-detect the bar pipeline with zero per-launch work.
typedef HRESULT (STDMETHODCALLTYPE *CreateGfxPSO_t)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
static CreateGfxPSO_t oCGPS = nullptr;

static uint64_t fnv1a(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p; uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static void store_pso_hash(void* pso, uint64_t h) {
    EnterCriticalSection(&g_psoCs);
    g_psoHash[pso] = h;
    if (g_barhash && h == g_barhash) g_barPSOs.insert(pso);
    if (g_boxhash && h == g_boxhash) g_boxPSOs.insert(pso);
    LeaveCriticalSection(&g_psoCs);
}
static HRESULT STDMETHODCALLTYPE hkCGPS(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** ppv) {
    HRESULT hr = oCGPS(self, d, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && d) {
        uint64_t h = 1469598103934665603ULL;
        if (d->VS.pShaderBytecode && d->VS.BytecodeLength) h ^= fnv1a(d->VS.pShaderBytecode, d->VS.BytecodeLength);
        if (d->PS.pShaderBytecode && d->PS.BytecodeLength) h ^= fnv1a(d->PS.pShaderBytecode, d->PS.BytecodeLength);
        store_pso_hash(*ppv, h);
    }
    return hr;
}

// ---- pipeline-STREAM creation hook (ID3D12Device2::CreatePipelineState) ----
// Many of AC6's PSOs (incl. the garage-entry fade-box overlay) are created via the
// stream API, whose desc holds no inline shader bytecode at a fixed offset. We walk
// the subobject stream, find the VS/PS bytecode, and hash it -> launch-stable id.
#define IDX_CreatePipelineState 47   // ID3D12Device2 vtable
typedef HRESULT (STDMETHODCALLTYPE *CreatePipelineState_t)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static CreatePipelineState_t oCPSO = nullptr;
static inline size_t align_up(size_t x, size_t a) { return (x + (a - 1)) & ~(a - 1); }
static uint64_t hash_pso_stream(const void* stream, size_t size) {
    const uint8_t* b = (const uint8_t*)stream; size_t pos = 0;
    uint64_t h = 1469598103934665603ULL;
    while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= size) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(b + pos);
        size_t isz, ial;
        switch (t) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: isz=sizeof(void*); ial=8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: isz=sizeof(D3D12_SHADER_BYTECODE); ial=8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: isz=sizeof(D3D12_STREAM_OUTPUT_DESC); ial=8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: isz=sizeof(D3D12_BLEND_DESC); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: isz=sizeof(UINT); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: isz=sizeof(D3D12_RASTERIZER_DESC); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: isz=sizeof(D3D12_DEPTH_STENCIL_DESC); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: isz=sizeof(D3D12_INPUT_LAYOUT_DESC); ial=8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: isz=sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: isz=sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: isz=sizeof(D3D12_RT_FORMAT_ARRAY); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: isz=sizeof(DXGI_FORMAT); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: isz=sizeof(DXGI_SAMPLE_DESC); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: isz=sizeof(UINT); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: isz=sizeof(D3D12_CACHED_PIPELINE_STATE); ial=8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: isz=sizeof(D3D12_PIPELINE_STATE_FLAGS); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: isz=sizeof(D3D12_DEPTH_STENCIL_DESC1); ial=4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: isz=sizeof(D3D12_VIEW_INSTANCING_DESC); ial=8; break;
            default: return h;   // unknown subobject -> stop walking (avoid desync)
        }
        size_t ioff = align_up(sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE), ial);
        const uint8_t* inner = b + pos + ioff;
        if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
            const D3D12_SHADER_BYTECODE* bc = (const D3D12_SHADER_BYTECODE*)inner;
            if (bc->pShaderBytecode && bc->BytecodeLength) h ^= fnv1a(bc->pShaderBytecode, bc->BytecodeLength);
        }
        pos += align_up(ioff + isz, sizeof(void*));
    }
    return h;
}
static HRESULT STDMETHODCALLTYPE hkCPSO(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** ppv) {
    HRESULT hr = oCPSO(self, d, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && d && d->pPipelineStateSubobjectStream && d->SizeInBytes)
        store_pso_hash(*ppv, hash_pso_stream(d->pPipelineStateSubobjectStream, d->SizeInBytes));
    return hr;
}

// ---- render-target resize (true Vert+): make 16:9 RTs 5:4 at creation ----
typedef HRESULT (STDMETHODCALLTYPE *CreateCommittedResource_t)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
static CreateCommittedResource_t oCCR = nullptr;

static HRESULT STDMETHODCALLTYPE hkCCR(
    ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        UINT w = (UINT)desc->Width, h = desc->Height;
        if (h > 0) {
            float ar = (float)w / (float)h;
            bool sixteen9 = fabsf(ar - (16.0f/9.0f)) < 0.02f;
            if (g_logmat)
                LG("[RT] %ux%u ar=%.3f fmt=%u flags=0x%x%s", w, h, ar, (unsigned)desc->Format,
                   (unsigned)desc->Flags, sixteen9 ? "  <16:9>" : "");
            // Resize EVERY 16:9 RT/DS to 5:4 — not just the full-res targets but the
            // whole downscale pyramid (640x360, 320x180, 160x90, 80x45 ...) and the
            // downsampled depth. Missing the pyramid was the mission corruption: a
            // fullscreen pass sampling a still-16:9 buffer onto a 5:4 target reads
            // out-of-range below the old 720 line -> foggy/see-through bottom. Sizes are
            // a clean power-of-two pyramid so W*4/5 keeps every level proportional.
            if (g_rtresize && sixteen9 && w >= 40) {
                D3D12_RESOURCE_DESC d2 = *desc;
                d2.Height = (UINT)(w / 1.25f + 0.5f);   // 16:9 -> 5:4
                if (g_logmat) LG("[RT] resize %ux%u -> %ux%u", w, h, w, (unsigned)d2.Height);
                return oCCR(self, hp, hf, &d2, st, cv, riid, ppv);
            }
        }
    }
    return oCCR(self, hp, hf, desc, st, cv, riid, ppv);
}

// ---- CreatePlacedResource (DIAGNOSTIC, log-only): are any 16:9 RT/DS buffers
//      allocated from a heap rather than committed? Those bypass hkCCR and stay 16:9.
//      Log-only for now (resizing a placed resource needs the heap/offsets to account
//      for the bigger size, else it overlaps the next resource -> GPU corruption).
typedef HRESULT (STDMETHODCALLTYPE *CreatePlacedResource_t)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
static CreatePlacedResource_t oCPR = nullptr;
static std::set<uint64_t> g_seenCPR;
static HRESULT STDMETHODCALLTYPE hkCPR(
    ID3D12Device* self, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    if (g_logmat && desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        UINT w = (UINT)desc->Width, h = desc->Height;
        if (h > 0) {
            float ar = (float)w / (float)h;
            bool s9 = fabsf(ar - (16.0f/9.0f)) < 0.02f;
            uint64_t k = key4(w, h, (int)desc->Format, (int)desc->Flags);
            if (g_seenCPR.insert(k).second)
                LG("[CPR] %ux%u ar=%.3f fmt=%u flags=0x%x%s", w, h, ar, (unsigned)desc->Format,
                   (unsigned)desc->Flags, s9 ? "  <16:9 PLACED!>" : "");
        }
    }
    return oCPR(self, heap, off, desc, st, cv, riid, ppv);
}

// Grab the command-list vtable via a throwaway device + allocator + list.
static bool get_cmdlist_vtable(void*** out_vtbl) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) { LG("no d3d12.dll"); return false; }
    auto pD3D12CreateDevice = (decltype(&D3D12CreateDevice))GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pD3D12CreateDevice) { LG("no D3D12CreateDevice"); return false; }

    ID3D12Device* dev = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) {
        LG("CreateDevice failed"); return false;
    }
    g_dev_vtbl = *reinterpret_cast<void***>(dev);   // for CreateCommittedResource/CreatePlacedResource hooks
    ID3D12CommandAllocator* alloc = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
        LG("CreateCommandAllocator failed"); dev->Release(); return false;
    }
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) {
        LG("CreateCommandList failed"); alloc->Release(); dev->Release(); return false;
    }
    *out_vtbl = *reinterpret_cast<void***>(list);

    // also grab the ID3D12Resource vtable from a throwaway upload buffer (for Map hook)
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 256; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))) && res) {
        g_res_vtbl = *reinterpret_cast<void***>(res);
        res->Release();
    } else LG("dummy upload buffer failed (no Map hook)");

    list->Release(); alloc->Release(); dev->Release();
    return true;
}

// Diagnostic: periodically sweep all tracked upload heaps for projection-shaped
// matrices (m00,m11 nonzero; col0/col1 off-diagonals zero) and log distinct
// (m00,m11,ratio). Read-only — tells us if a clean live projection exists and at
// what ratio, regardless of how it's bound.
static std::set<uint64_t> g_seenProj;
static DWORD WINAPI scan_thread(LPVOID) {
    for (;;) {
        Sleep(500);
        EnterCriticalSection(&g_cs);
        long n = g_nreg;
        LeaveCriticalSection(&g_cs);
        for (long i = 0; i < n; i++) {
            EnterCriticalSection(&g_cs);
            uintptr_t base = g_regions[i].cpu_base; unsigned long long sz = g_regions[i].size;
            LeaveCriticalSection(&g_cs);
            if (!base || sz < 64) continue;
            unsigned long long cap = sz/4; if (cap > 65536) cap = 65536;  // scan up to 256KB/region
            float* f = (float*)base;
            for (unsigned long long k = 0; k + 16 <= cap; k++) {
                float m00=f[k], m11=f[k+5];
                if (m00==0.0f||m11==0.0f) continue;
                if (f[k+1]||f[k+2]||f[k+3]||f[k+4]||f[k+6]||f[k+7]) continue;
                float r = m00/m11;
                if (r>0.3f && r<0.95f) {
                    uint64_t key = ((uint64_t)(int)(r*1000)<<32) ^ (uint64_t)(int)(m11*1000);
                    if (g_seenProj.insert(key).second)
                        LG("SCAN proj-like: m00=%.5f m11=%.5f ratio=%.5f (region#%ld off=%llu)", m00, m11, r, i, k*4);
                }
            }
        }
    }
    return 0;
}

// ============ frame capture (Present hook) ============
// Grabs the game's present queue (via ExecuteCommandLists), hooks Present, and
// every CAP_EVERY frames copies the backbuffer to a readback buffer and dumps
// raw bytes to Game/ac6cap.bin (header: magic,w,h,rowPitch,dxgiFormat). A python
// helper converts to PNG. Lets us SEE the rendered frame without a display.
typedef void (STDMETHODCALLTYPE *ExecCL_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);
static ExecCL_t  oExecCL  = nullptr;
static Present_t oPresent = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
#define IDX_ExecuteCommandLists 10
#define IDX_Present 8
// g_frame declared up top (control block)
// g_capEvery declared up top (control block)
static volatile long g_capReq = 0;   // set >0 to force a capture on next present

static void STDMETHODCALLTYPE hkExecCL(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC qd = self->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { g_queue = self; LG("[cap] got present queue %p", (void*)self); }
    }
    oExecCL(self, n, lists);
}

static void dump_backbuffer(IDXGISwapChain* sc) {
    if (!g_queue) return;
    IDXGISwapChain3* sc3 = nullptr;
    UINT idx = 0;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) { idx = sc3->GetCurrentBackBufferIndex(); sc3->Release(); }
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(bb->GetDevice(IID_PPV_ARGS(&dev))) || !dev) { bb->Release(); return; }
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);

    ID3D12Resource* rb = nullptr;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = total; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) { dev->Release(); bb->Release(); return; }
    ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fc));
    if (al && cl && fc) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[1] = { cl };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(fc, 1);
        if (fc->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            fc->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 2000); CloseHandle(ev);
        }
        void* p = nullptr; D3D12_RANGE rr{0, (SIZE_T)total};
        if (SUCCEEDED(rb->Map(0, &rr, &p)) && p) {
            FILE* f = fopen("ac6cap.bin", "wb");
            if (f) {
                unsigned int hdr[5] = { 0xAC6CA9, (unsigned)d.Width, (unsigned)d.Height, (unsigned)fp.Footprint.RowPitch, (unsigned)d.Format };
                fwrite(hdr, sizeof(hdr), 1, f);
                fwrite(p, 1, (size_t)(fp.Footprint.RowPitch * (UINT64)d.Height), f);
                fclose(f);
                LG("[cap] dumped %ux%u fmt=%u pitch=%u", (unsigned)d.Width, (unsigned)d.Height, (unsigned)d.Format, (unsigned)fp.Footprint.RowPitch);
            }
            D3D12_RANGE wr{0,0}; rb->Unmap(0, &wr);
        }
    }
    if (fc) fc->Release(); if (cl) cl->Release(); if (al) al->Release(); rb->Release();
    dev->Release(); bb->Release();
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    long fr = InterlockedIncrement(&g_frame);
    // bisection: snapshot + reset the per-frame draw ordinal
    g_lastDrawN = g_drawOrd; g_drawOrd = 0;
    if (g_skipord && (fr % 30) == 0) LG("FRAME fr=%ld draws=%ld skip=[%d,%d)", fr, g_lastDrawN, g_ordlo, g_ordhi);
    // new frame: reset per-frame CB-scaled set; reload live controls periodically
    EnterCriticalSection(&g_csf); g_scaledFrame.clear(); LeaveCriticalSection(&g_csf);
    if ((fr % 30) == 0) {
        int prevScan = g_aspscan, prevPatch = g_asppatch, prevFreeze = g_psofreeze;
        load_ctl();
        // live rising-edge: set aspscan/asppatch=1 in ac6ctl.txt to (re)run the hunter
        // NOW (e.g. while in the garage, so its Arxan-decrypted code is captured).
        if ((g_aspscan && !prevScan) || (g_asppatch && !prevPatch))
            CreateThread(nullptr, 0, aspect_scan_thread, nullptr, 0, nullptr);
        // reversible UI-lever test: re-apply when pfval/pfnew change in the ctl file
        if (g_pfval != g_pfvalApplied || g_pfnew != g_pfnewApplied)
            apply_pf_patch();
        // PSO-bisection: rising edge on psofreeze -> snapshot the distinct-PSO set into
        // a sorted vector so frozen indices are stable for binary search.
        if (g_psofreeze && !prevFreeze) {
            EnterCriticalSection(&g_psoCs);
            g_psoFrozen.assign(g_psoSeen.begin(), g_psoSeen.end()); // std::set already sorted
            LeaveCriticalSection(&g_psoCs);
            LG("PSOFREEZE count=%zu  (indices 0..%zu)", g_psoFrozen.size(),
               g_psoFrozen.empty()?0:g_psoFrozen.size()-1);
        }
        // barhash changed -> (re)build the matched-PSO set from the full hash map (handles
        // PSOs created before barhash was set in the ctl, and is idempotent).
        static uint64_t s_lastBar = 0;
        if (g_barhash != s_lastBar) {
            s_lastBar = g_barhash;
            EnterCriticalSection(&g_psoCs);
            g_barPSOs.clear();
            if (g_barhash) for (auto& kv : g_psoHash) if (kv.second == g_barhash) g_barPSOs.insert(kv.first);
            size_t nm = g_barPSOs.size();
            LeaveCriticalSection(&g_psoCs);
            LG("BARHASH 0x%llx matched %zu PSO(s)", (unsigned long long)g_barhash, nm);
        }
        // drawn-PSO diff: snapshot steady set / dump the box-only difference
        static int s_lastSnap = 0, s_lastDiff = 0;
        if (g_drawsnap && !s_lastSnap) {
            EnterCriticalSection(&g_psoCs);
            g_drawnSteady = g_drawnPSO; g_drawnPSO.clear();
            size_t n = g_drawnSteady.size();
            LG("DRAWSNAP steady=%zu PSOs (cleared accum)", n);
            for (void* p : g_drawnSteady) {
                uint64_t h = g_psoHash.count(p) ? g_psoHash[p] : 0ULL;
                LG("STEADY low32=0x%08x hash=0x%llx", (unsigned)(uintptr_t)p, (unsigned long long)h);
            }
            LeaveCriticalSection(&g_psoCs);
        }
        s_lastSnap = g_drawsnap;
        if (g_drawdiff && !s_lastDiff) {
            EnterCriticalSection(&g_psoCs);
            LG("DRAWDIFF start (fullscreen-quad PSOs: drawn=%zu steady=%zu) ----", g_drawnPSO.size(), g_drawnSteady.size());
            for (void* p : g_drawnPSO) {
                uint64_t h = g_psoHash.count(p) ? g_psoHash[p] : 0ULL;
                bool transient = !g_drawnSteady.count(p);
                LG("DRAWDIFF ptr=%p low32=0x%08x hash=0x%llx %s", p,
                   (unsigned)(uintptr_t)p, (unsigned long long)h, transient ? "TRANSIENT" : "steady");
            }
            LG("DRAWDIFF end ----");
            LeaveCriticalSection(&g_psoCs);
        }
        s_lastDiff = g_drawdiff;
        static uint64_t s_lastBox = 0;
        if (g_boxhash != s_lastBox) {
            s_lastBox = g_boxhash;
            EnterCriticalSection(&g_psoCs);
            g_boxPSOs.clear();
            if (g_boxhash) for (auto& kv : g_psoHash) if (kv.second == g_boxhash) g_boxPSOs.insert(kv.first);
            size_t nm = g_boxPSOs.size();
            LeaveCriticalSection(&g_psoCs);
            LG("BOXHASH 0x%llx matched %zu PSO(s)", (unsigned long long)g_boxhash, nm);
        }
        // diag: reveal the pinned frozen PSO's pointer + its shader hash (for capturing barhash)
        if (g_pinhash && g_psklo >= 0 && (size_t)g_psklo < g_psoFrozen.size()) {
            void* p = g_psoFrozen[g_psklo];
            EnterCriticalSection(&g_psoCs);
            uint64_t h = g_psoHash.count(p) ? g_psoHash[p] : 0ULL;
            LeaveCriticalSection(&g_psoCs);
            LG("PINHASH frozen[%d] ptr=%p hash=0x%llx", g_psklo, p, (unsigned long long)h);
        }
    }
    if (g_queue && (g_capReq > 0 || (g_capEvery > 0 && (fr % g_capEvery) == 0))) {
        if (g_capReq > 0) InterlockedDecrement(&g_capReq);
        dump_backbuffer(sc);
    }
    return oPresent(sc, sync, flags);
}

// dummy swapchain (hidden window) just to read the IDXGISwapChain::Present vtable
static void** get_swapchain_vtable(ID3D12CommandQueue* q) {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ac6cap";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ac6cap", L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LG("[cap] no hwnd"); return nullptr; }
    IDXGIFactory2* fac = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac))) || !fac) { LG("[cap] no factory"); DestroyWindow(hwnd); return nullptr; }
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 16; sd.Height = 16; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    void** vt = nullptr;
    if (SUCCEEDED(fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc)) && sc) {
        vt = *reinterpret_cast<void***>(sc); sc->Release();
    } else LG("[cap] CreateSwapChainForHwnd failed");
    fac->Release(); DestroyWindow(hwnd);
    return vt;
}

static void** g_queue_vtbl = nullptr;
static void** g_swapchain_vtbl = nullptr;
static void get_capture_vtables() {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto pCreate = (decltype(&D3D12CreateDevice))GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pCreate) { LG("[cap] no D3D12CreateDevice"); return; }
    ID3D12Device* dev = nullptr;
    if (FAILED(pCreate(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) { LG("[cap] no dev"); return; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) {
        g_queue_vtbl = *reinterpret_cast<void***>(q);
        g_swapchain_vtbl = get_swapchain_vtable(q);
        q->Release();
    } else LG("[cap] no queue");
    dev->Release();
}

// ---- ASPECT-CLAMP HUNTER ----------------------------------------------------
// AC6 renders ANY aspect natively but clamps the narrow/tall side:
//   renderAspect = max(displayAspect, 16/9).
// Find that clamp by locating the 16/9 (0x3FE38E39) and 9/16 (0x3F100000) float
// constants in the module's data, then scanning executable bytes for any
// RIP-relative reference to them, logging the surrounding opcode (MAXSS=F3 0F 5F,
// MINSS=F3 0F 5D, COMISS=0F 2F, UCOMISS=0F 2E, MOVSS=F3 0F 10 ...). Runtime scan
// because the exe is Arxan-protected (decrypted only in memory).
static bool region_readable(void* p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.Protect & bad) return false;
    return ((char*)p + n) <= ((char*)mbi.BaseAddress + mbi.RegionSize);
}
static const char* op_name(const unsigned char* op, int n) {
    // op points a few bytes before the disp32; sniff the SSE opcode just before the modrm
    for (int i = 0; i < n - 1; i++) {
        if (op[i] == 0x0F) {
            unsigned char o = op[i+1];
            if (o == 0x5F) return "MAXSS/PS";
            if (o == 0x5D) return "MINSS/PS";
            if (o == 0x2F) return "COMISS";
            if (o == 0x2E) return "UCOMISS";
            if (o == 0x10) return "MOVSS-load";
            if (o == 0x59) return "MULSS";
            if (o == 0x5E) return "DIVSS";
            if (o == 0x58) return "ADDSS";
        }
    }
    return "?";
}
// known aspect-related float constants
#define C16_9 0x3FE38E39u   // 1.7777778f
#define C9_16 0x3F100000u   // 0.5625f
#define C16_0 0x41800000u   // 16.0f
#define C9_0  0x41100000u   // 9.0f
#define C1920 0x44F00000u   // 1920.0f (UI reference-canvas width)
#define C1080 0x44870000u   // 1080.0f (UI reference-canvas height)
// g_uiscan (declared with the control block up top): when set, the scanner ALSO
// collects/xrefs the UI-canvas dims (1920/1080) to find where the 2D UI computes its
// 16:9 letterbox. Off by default (noisy).
static const char* asp_label(unsigned v) {
    switch (v) { case C16_9: return "16/9"; case C9_16: return "9/16";
                 case C16_0: return "16.0"; case C9_0: return "9.0";
                 case C1920: return g_uiscan ? "1920" : nullptr;
                 case C1080: return g_uiscan ? "1080" : nullptr;
                 default: return nullptr; }
}

// ---- reversible by-value float patch (UI lever hunt) ----
// patch every 4-byte float == want in [rstart,rend) to repl; record originals. SAFE region
// assumed (caller filters). Returns count.
static int pf_scan_region(char* rstart, char* rend, unsigned want, unsigned repl) {
    int n = 0;
    for (char* q = rstart; q + 4 <= rend; q += 4) {
        if (*(unsigned*)q == want) {
            DWORD o; if (VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &o)) {
                g_pfRecord.push_back({(unsigned*)q, want});
                *(unsigned*)q = repl; VirtualProtect(q, 4, o, &o); n++;
            }
        }
    }
    return n;
}
static void apply_pf_patch() {
    // restore any previous patch first (so each test starts clean / pfval=0 reverts).
    // re-VirtualQuery each recorded addr — heap allocs may have been freed since (don't
    // write into unmapped memory).
    if (!g_pfRecord.empty()) {
        int rc = 0; MEMORY_BASIC_INFORMATION q;
        for (auto& pr : g_pfRecord) {
            if (!VirtualQuery(pr.first, &q, sizeof(q))) continue;
            if (q.State != MEM_COMMIT || (q.Protect & (PAGE_NOACCESS|PAGE_GUARD))) continue;
            DWORD o; if (VirtualProtect(pr.first, 4, PAGE_EXECUTE_READWRITE, &o)) {
                *pr.first = pr.second; VirtualProtect(pr.first, 4, o, &o); rc++;
            }
        }
        LG("[pf] restored %d/%d floats", rc, (int)g_pfRecord.size());
        g_pfRecord.clear();
    }
    g_pfvalApplied = g_pfval; g_pfnewApplied = g_pfnew;
    if (g_pfval == 0.0f) return;             // off
    unsigned want = *(unsigned*)&g_pfval, repl = *(unsigned*)&g_pfnew;
    HMODULE base = GetModuleHandleA(NULL); if (!base) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)base + dos->e_lfanew);
    char* lo = (char*)base, *hi = lo + nt->OptionalHeader.SizeOfImage;
    MEMORY_BASIC_INFORMATION mbi; int nimg = 0, nheap = 0;
    // pass A: the module image (data/rdata/bss) — always
    for (char* p = lo; p < hi; ) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
        if (rend > hi) rend = hi;
        bool readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD));
        if (readable) nimg += pf_scan_region(rstart, rend, want, repl);
        p = rstart + mbi.RegionSize;
    }
    // pass B (g_pfheap): writable heap (MEM_PRIVATE) across the whole address space.
    // TIGHT FILTERS to dodge the D3D-upload-heap unmap crash seen before:
    //  - MEM_COMMIT + MEM_PRIVATE only (skip MEM_MAPPED files & MEM_IMAGE)
    //  - PAGE_READWRITE / WRITECOPY only (skip exec, readonly, guard, noaccess)
    //  - skip regions > 64MB (GPU/asset heaps) and the module image (done above)
    if (g_pfheap) {
        char* addr = (char*)0x10000;
        char* end  = (char*)0x7FFFFFFE0000ULL;
        while (addr < end) {
            if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
            char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
            bool ok = (mbi.State == MEM_COMMIT) && (mbi.Type == MEM_PRIVATE) &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) &&
                      (mbi.RegionSize <= (64ULL<<20)) &&
                      !(rstart >= lo && rstart < hi);   // module image already scanned
            if (ok) nheap += pf_scan_region(rstart, rend, want, repl);
            addr = rstart + mbi.RegionSize;
        }
    }
    LG("[pf] patched img=%d heap=%d  %.4f -> %.4f", nimg, nheap, g_pfval, g_pfnew);
}

// Patch the live aspect-clamp constants (16/9 and/or 9/16) to 1.0f per g_asppatch.
// Returns count overwritten. Self-contained (re-derives the module range) so it can be
// called repeatedly from an early poll loop. Re-find-by-value = robust to ASLR/offsets.
static int patch_aspect_now() {
    if (!g_asppatch) return 0;
    HMODULE base = GetModuleHandleA(NULL);
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)base + dos->e_lfanew);
    char* lo = (char*)base, *hi = lo + nt->OptionalHeader.SizeOfImage;
    unsigned NEWV = 0x3F800000; // 1.0f
    int patched = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* p = lo; p < hi; ) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
        if (rend > hi) rend = hi;
        bool readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD));
        if (readable) {
            for (char* q = rstart; q + 4 <= rend; q += 4) {
                unsigned v = *(unsigned*)q;
                bool hit16 = (v == C16_9) && (g_asppatch == 1 || g_asppatch == 3);
                bool hit9  = (v == C9_16) && (g_asppatch == 2 || g_asppatch == 3);
                if (hit16 || hit9) {
                    DWORD old;
                    if (VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &old)) {
                        *(unsigned*)q = NEWV;
                        VirtualProtect(q, 4, old, &old);
                        patched++;
                    }
                }
            }
        }
        p = rstart + mbi.RegionSize;
    }
    return patched;
}

// AUTO-APPLY: spawned from DllMain (process load) — the EARLIEST point we can run, before
// the engine's main() and (hopefully) before its first renderAspect compute. Poll from t=0
// and patch the aspect constants the INSTANT Arxan decrypts them, so the game boots native
// 5:4 with NO manual resolution-change. Self-sufficient: reads asppatch from
// <moduledir>\ac6ctl.txt by ABSOLUTE path (does NOT touch CWD — the engine is still doing
// its own startup file IO here; init_thread sets CWD later). mode 3 expects 4 consts.
static DWORD WINAPI early_patch_thread(LPVOID) {
    char path[MAX_PATH] = {0};
    if (g_self && GetModuleFileNameA(g_self, path, MAX_PATH)) {
        char* bs = strrchr(path, '\\'); if (bs) strcpy(bs + 1, "ac6ctl.txt");
    }
    int mode = g_asppatch;   // PROD: baked default (3); ctl may override below
    FILE* f = fopen(path, "r");
    if (f) { char ln[160]; float v; while (fgets(ln, sizeof(ln), f)) if (sscanf(ln, "asppatch=%f", &v) == 1) mode = (int)v; fclose(f); }
    if (!mode) return 0;
    g_asppatch = mode;                          // so patch_aspect_now() acts (init_thread's load_ctl re-confirms)
    int want = (mode == 3) ? 4 : 2;
    for (int i = 0; i < 1200; i++) {            // up to ~24s @ 20ms (covers slow Arxan unpack)
        int p = patch_aspect_now();
        if (p >= want) {
            LG("[asp] EARLY PATCH %d consts at poll #%d (~%dms)", p, i, i * 20);
            for (int j = 0; j < 8; j++) { Sleep(100); patch_aspect_now(); } // catch late dupes
            return 0;
        }
        Sleep(20);
    }
    LG("[asp] EARLY PATCH: never found %d consts in ~24s", want);
    return 0;
}

static DWORD WINAPI aspect_scan_thread(LPVOID) {
    Sleep(2000);   // brief settle; on-demand trigger means the relevant code is already decrypted
    HMODULE base = GetModuleHandleA(NULL);
    if (!base) { LG("[asp] no main module"); return 0; }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)base + dos->e_lfanew);
    SIZE_T imgsz = nt->OptionalHeader.SizeOfImage;
    char* lo = (char*)base, *hi = lo + imgsz;
    LG("[asp] module=%p size=%llu  scanning for 16/9=0x3FE38E39 / 9:16=0x3F100000", base, (unsigned long long)imgsz);

    // pass 1: collect addresses of the constants
    char* consts[512]; unsigned cval[512]; int nc = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* p = lo; p < hi; ) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
        if (rend > hi) rend = hi;
        bool readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD));
        if (readable) {
            for (char* q = rstart; q + 4 <= rend; q += 4) {
                unsigned v = *(unsigned*)q;
                if (asp_label(v) && nc < 512) { consts[nc] = q; cval[nc] = v; nc++; }
            }
        }
        p = rstart + mbi.RegionSize;
    }
    LG("[asp] found %d constant locations", nc);
    for (int i = 0; i < nc && i < 60; i++)
        LG("[asp]   const @ +0x%llx = %s", (unsigned long long)(consts[i]-lo), asp_label(cval[i]));

    // pass 2: scan executable regions for rip-relative refs to those constants
    // also dump a 160-byte window around each hit to ac6asp.bin for offline capstone:
    //   record = [u64 window_start_module_offset][u32 len][len bytes]
    FILE* df = fopen("ac6asp.bin", "wb");
    int hits = 0;
    for (char* p = lo; p < hi; ) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
        if (rend > hi) rend = hi;
        bool exec = (mbi.State == MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
                    !(mbi.Protect & PAGE_GUARD);
        if (exec) {
            for (char* q = rstart; q + 4 <= rend; q++) {
                int disp = *(int*)q;
                char* target = q + 4 + disp;   // rip = end of disp32 (approx instr end)
                if (target < consts[0] - 0) {} // cheap noop to keep target used
                // membership test
                for (int i = 0; i < nc; i++) {
                    if (target == consts[i]) {
                        if (hits > 400) { hits++; break; }   // cap noise (16.0/9.0 are common)
                        const unsigned char* op = (const unsigned char*)(q - 6);
                        LG("[asp] HIT @ +0x%llx op=%s -> %s  bytes: %02x %02x %02x %02x %02x %02x|%02x %02x %02x %02x",
                           (unsigned long long)((q-4)-lo) + 4, op_name(op, 7), asp_label(cval[i]),
                           op[0],op[1],op[2],op[3],op[4],op[5], (unsigned char)q[0],(unsigned char)q[1],(unsigned char)q[2],(unsigned char)q[3]);
                        if (df) {
                            char* ws = q - 64;     // window: 64 bytes before disp32 .. 96 after
                            unsigned len = 160;
                            if (region_readable(ws, len)) {
                                unsigned long long woff = (unsigned long long)(ws - lo);
                                fwrite(&woff, 8, 1, df); fwrite(&len, 4, 1, df); fwrite(ws, 1, len, df);
                            }
                        }
                        hits++;
                        break;
                    }
                }
            }
        }
        p = rstart + mbi.RegionSize;
    }
    if (df) fclose(df);
    LG("[asp] DONE  %d rip-relative refs to the aspect constants (windows -> ac6asp.bin)", hits);

    // ---- experimental aspect-clamp PATCH (g_asppatch) ----
    // mode 1: lower the 16/9 threshold constants below 1.25 -> 5:4 reads as "not narrow"
    // mode 3: also raise the 9/16 reciprocal constants above 0.8
    // (data patch; re-find by value so it's robust). value 1.0f = 0x3F800000.
    if (g_asppatch) {
        unsigned NEWV = 0x3F800000; // 1.0f
        int patched = 0;
        for (char* p = lo; p < hi; ) {
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            char* rstart = (char*)mbi.BaseAddress, *rend = rstart + mbi.RegionSize;
            if (rend > hi) rend = hi;
            bool readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD));
            if (readable) {
                for (char* q = rstart; q + 4 <= rend; q += 4) {
                    unsigned v = *(unsigned*)q;
                    bool hit16 = (v == C16_9) && (g_asppatch == 1 || g_asppatch == 3);
                    bool hit9  = (v == C9_16) && (g_asppatch == 2 || g_asppatch == 3);
                    if (hit16 || hit9) {
                        DWORD old;
                        if (VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &old)) {
                            *(unsigned*)q = NEWV;
                            VirtualProtect(q, 4, old, &old);
                            patched++;
                            LG("[asp] PATCH @ +0x%llx  %s -> 1.0", (unsigned long long)(q-lo), hit16?"16/9":"9/16");
                        }
                    }
                }
            }
            p = rstart + mbi.RegionSize;
        }
        LG("[asp] PATCH done: %d constants overwritten (mode %d)", patched, g_asppatch);
    }
    return 0;
}

static DWORD WINAPI init_thread(LPVOID) {
    // Derive the Game dir from our own module path and pin CWD/DLL-search there, so
    // the log, ac6ctl.txt, and any sidecar plugin (AC6CameraTweaks + MinHook.x64.dll +
    // its .ini) all resolve regardless of the process's launch-time CWD.
    char gamedir[MAX_PATH] = {0};
    if (g_self && GetModuleFileNameA(g_self, gamedir, MAX_PATH)) {
        char* bs = strrchr(gamedir, '\\'); if (bs) *bs = '\0';
        SetCurrentDirectoryA(gamedir);
        SetDllDirectoryA(gamedir);
    }
    g_log = fopen("ac6vp.log", "w");
    LG("[ac6-vertplus] init  gamedir=%s", gamedir);
    // Optional sidecar: if AC6CameraTweaks.dll is present, load it (it self-hooks the
    // camera FOV). Absent in clean/online mode -> nothing loads. LoadLibrary searches
    // the dir we just set, so its MinHook.x64.dll / lua51.dll / .ini are found.
    {
        char ctp[MAX_PATH];
        snprintf(ctp, sizeof(ctp), "%s\\AC6CameraTweaks.dll", gamedir);
        if (GetFileAttributesA(ctp) != INVALID_FILE_ATTRIBUTES) {
            HMODULE m = LoadLibraryA(ctp);
            LG("[ac6-vertplus] AC6CameraTweaks load = %p (err=%lu)", (void*)m, GetLastError());
        }
    }
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_csf);
    InitializeCriticalSection(&g_psoCs);
    load_ctl();
    if (MH_Initialize() != MH_OK) { LG("MH_Initialize failed"); return 0; }
    void** vtbl = nullptr;
    if (!get_cmdlist_vtable(&vtbl)) { LG("vtable grab failed"); return 0; }
    LG("[ac6-vertplus] got cmdlist vtable %p", (void*)vtbl);

    if (MH_CreateHook(vtbl[IDX_RSSetViewports], (void*)&hkRSSetViewports, (void**)&oRSSetViewports) != MH_OK)
        LG("hook RSSetViewports failed");
    if (MH_CreateHook(vtbl[IDX_RSSetScissorRects], (void*)&hkRSSetScissorRects, (void**)&oRSSetScissorRects) != MH_OK)
        LG("hook RSSetScissorRects failed");
    // letterbox-hunt (Phase 3): draw/clear/PSO/topology hooks for finding+skipping the 2D bars
    if (MH_CreateHook(vtbl[IDX_DrawInstanced], (void*)&hkDrawInstanced, (void**)&oDrawInstanced) != MH_OK)
        LG("hook DrawInstanced failed");
    if (MH_CreateHook(vtbl[IDX_DrawIndexedInstanced], (void*)&hkDrawIndexedInstanced, (void**)&oDrawIndexedInstanced) != MH_OK)
        LG("hook DrawIndexedInstanced failed");
    if (MH_CreateHook(vtbl[IDX_IASetPrimitiveTopology], (void*)&hkIASetPrimitiveTopology, (void**)&oIASetPrimitiveTopology) != MH_OK)
        LG("hook IASetPrimitiveTopology failed");
    if (MH_CreateHook(vtbl[IDX_SetPipelineState], (void*)&hkSetPipelineState, (void**)&oSetPipelineState) != MH_OK)
        LG("hook SetPipelineState failed");
    if (MH_CreateHook(vtbl[IDX_ClearRenderTargetView], (void*)&hkClearRTV, (void**)&oClearRTV) != MH_OK)
        LG("hook ClearRenderTargetView failed");
#ifdef ENABLE_VERTPLUS
    // projection clip-Y scaling: hook CBV bind + Map/Unmap (VA->CPU tracking).
    if (MH_CreateHook(vtbl[IDX_SetGraphicsRootConstantBufferView], (void*)&hkSetGfxRootCBV, (void**)&oSetGfxRootCBV) != MH_OK)
        LG("hook SetGraphicsRootConstantBufferView failed");
    if (g_res_vtbl) {
        if (MH_CreateHook(g_res_vtbl[IDX_Map], (void*)&hkMap, (void**)&oMap) != MH_OK)
            LG("hook Map failed");
        if (MH_CreateHook(g_res_vtbl[IDX_Unmap], (void*)&hkUnmap, (void**)&oUnmap) != MH_OK)
            LG("hook Unmap failed");
    }
    if (g_dev_vtbl) {
        if (MH_CreateHook(g_dev_vtbl[IDX_CreateCommittedResource], (void*)&hkCCR, (void**)&oCCR) != MH_OK)
            LG("hook CreateCommittedResource failed");
        if (MH_CreateHook(g_dev_vtbl[IDX_CreatePlacedResource], (void*)&hkCPR, (void**)&oCPR) != MH_OK)
            LG("hook CreatePlacedResource failed");
        if (MH_CreateHook(g_dev_vtbl[IDX_CreateGraphicsPipelineState], (void*)&hkCGPS, (void**)&oCGPS) != MH_OK)
            LG("hook CreateGraphicsPipelineState failed");
        if (MH_CreateHook(g_dev_vtbl[IDX_CreatePipelineState], (void*)&hkCPSO, (void**)&oCPSO) != MH_OK)
            LG("hook CreatePipelineState failed");
    }
#endif
#ifdef ENABLE_CAPTURE
    get_capture_vtables();
    if (g_queue_vtbl)
        if (MH_CreateHook(g_queue_vtbl[IDX_ExecuteCommandLists], (void*)&hkExecCL, (void**)&oExecCL) != MH_OK)
            LG("[cap] hook ExecuteCommandLists failed");
    if (g_swapchain_vtbl)
        if (MH_CreateHook(g_swapchain_vtbl[IDX_Present], (void*)&hkPresent, (void**)&oPresent) != MH_OK)
            LG("[cap] hook Present failed");
    else LG("[cap] no swapchain vtable");
#endif
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) LG("enable hooks failed");
    else LG("[ac6-vertplus] hooks enabled");
    // early_patch_thread is spawned from DllMain (earliest). Here only the diagnostic scan.
    if (g_aspscan)  CreateThread(nullptr, 0, aspect_scan_thread, nullptr, 0, nullptr);
#ifdef ENABLE_PROJ_RND
    CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr);
#endif
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        // earliest possible: start the aspect-constant poll-patch NOW (process load), to
        // race the engine's first renderAspect compute (auto-apply, no manual res-change).
        CreateThread(nullptr, 0, early_patch_thread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
