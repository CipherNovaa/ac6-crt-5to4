#!/usr/bin/env python3
# Read ac6cap.bin (backbuffer dump from the AC6 hook) and report mean luma for the
# letterbox-bar rows vs a known-scene row. Used to AUTOMATE the PSO bisection:
#   bar rows ~black (low luma)  -> bars PRESENT
#   bar rows ~scene (higher luma/variance) -> bars GONE
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "ac6cap.bin"
data = open(path, "rb").read()
magic, w, h, pitch, fmt = struct.unpack_from("<5I", data, 0)
hdr = 20
if magic != 0xAC6CA9:
    print("BADMAGIC 0x%x" % magic); sys.exit(2)

# bytes-per-pixel + channel extractor by DXGI format
def px_rgb(off):
    if fmt in (28, 87):              # R8G8B8A8 / B8G8R8A8 UNORM
        b0, b1, b2 = data[hdr+off], data[hdr+off+1], data[hdr+off+2]
        if fmt == 87:  # BGRA
            return b2, b1, b0
        return b0, b1, b2
    if fmt == 10:                    # R16G16B16A16_FLOAT (HDR) -> approx via high byte
        import struct as _s
        r = _s.unpack_from("<e", data, hdr+off)[0]
        g = _s.unpack_from("<e", data, hdr+off+2)[0]
        b = _s.unpack_from("<e", data, hdr+off+4)[0]
        return min(255,max(0,r*255)), min(255,max(0,g*255)), min(255,max(0,b*255))
    if fmt == 24:                    # R10G10B10A2_UNORM
        v = struct.unpack_from("<I", data, hdr+off)[0]
        r = (v & 0x3FF) >> 2; g = ((v>>10)&0x3FF)>>2; b = ((v>>20)&0x3FF)>>2
        return r, g, b
    # fallback: treat as 4-byte RGBA
    return data[hdr+off], data[hdr+off+1], data[hdr+off+2]

bpp = 8 if fmt == 10 else 4

def row_stats(y, x0, x1):
    lum = []
    for x in range(x0, x1, 4):
        off = y*pitch + x*bpp
        r,g,b = px_rgb(off)
        lum.append(0.299*r + 0.587*g + 0.114*b)
    n = len(lum); m = sum(lum)/n
    var = sum((v-m)**2 for v in lum)/n
    return m, var

# sample the RIGHT half (x 900..1180) to dodge the left menu / center panel
X0, X1 = 900, min(1180, w)
rows = {
    "scene_top(y=55)":  55,
    "TOPBAR(y=130)":    130,
    "TOPBAR(y=140)":    140,
    "BOTBAR(y=898)":    898,
    "BOTBAR(y=908)":    908,
    "scene_bot(y=975)": 975,
}
verdict = len(sys.argv) > 2 and sys.argv[2] == "--verdict"
if not verdict:
    print("cap %dx%d pitch=%d fmt=%d  (x %d..%d)" % (w, h, pitch, fmt, X0, X1))
    for name, y in rows.items():
        if y >= h: continue
        m, var = row_stats(y, X0, X1)
        print("  %-16s mean=%6.1f var=%9.1f" % (name, m, var))
else:
    tb = max(row_stats(130, X0, X1)[0], row_stats(140, X0, X1)[0])  # brightest of 2 top rows
    bb = max(row_stats(898, X0, X1)[0], row_stats(908, X0, X1)[0])
    sc = max(row_stats(55, X0, X1)[0], row_stats(975, X0, X1)[0])    # scene reference
    bar = max(tb, bb)
    # bars present = both bar rows black; gone = bar rows show (non-black) scene
    if sc < 4:
        v = "AMBIG"   # whole region dark, can't judge
    elif bar < 4:
        v = "PRESENT"
    else:
        v = "GONE"
    print("%s tb=%.1f bb=%.1f scene=%.1f" % (v, tb, bb, sc))
