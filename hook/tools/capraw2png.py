#!/usr/bin/env python3
# Convert the hook's raw backbuffer dump (Game/ac6cap.bin) to PNG.
# Header: uint32 magic(0xAC6CA9), width, height, rowPitch, dxgiFormat. Then pixels.
import sys, struct
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else "ac6cap.bin"
out  = sys.argv[2] if len(sys.argv) > 2 else "ac6cap.png"
d = open(path, "rb").read()
magic, w, h, pitch, fmt = struct.unpack_from("<IIIII", d, 0)
assert magic == 0xAC6CA9, f"bad magic {magic:#x}"
px = d[20:]
print(f"{w}x{h} pitch={pitch} dxgiFormat={fmt}")

img = Image.new("RGB", (w, h))
op = img.load()

def rows(bytes_per_px):
    for y in range(h):
        base = y * pitch
        yield y, base, bytes_per_px

if fmt in (28, 29):          # R8G8B8A8_UNORM(_SRGB)
    for y, base, _ in rows(4):
        for x in range(w):
            o = base + x*4; op[x, y] = (px[o], px[o+1], px[o+2])
elif fmt in (87, 91):        # B8G8R8A8_UNORM(_SRGB)
    for y, base, _ in rows(4):
        for x in range(w):
            o = base + x*4; op[x, y] = (px[o+2], px[o+1], px[o])
elif fmt == 24:              # R10G10B10A2_UNORM
    for y in range(h):
        base = y*pitch
        for x in range(w):
            v = struct.unpack_from("<I", px, base + x*4)[0]
            r = (v & 0x3FF); g = (v>>10) & 0x3FF; b = (v>>20) & 0x3FF
            op[x, y] = (r*255//1023, g*255//1023, b*255//1023)
elif fmt == 10:              # R16G16B16A16_FLOAT (HDR) -> tonemap-ish clamp
    import struct as _s
    def half(u):
        s=(u>>15)&1; e=(u>>10)&0x1f; m=u&0x3ff
        if e==0: f=(m/1024.0)*2**-14
        elif e==31: f=65504.0
        else: f=(1+m/1024.0)*2**(e-15)
        return -f if s else f
    for y in range(h):
        base=y*pitch
        for x in range(w):
            o=base+x*8
            r=half(_s.unpack_from('<H',px,o)[0]); g=half(_s.unpack_from('<H',px,o+2)[0]); b=half(_s.unpack_from('<H',px,o+4)[0])
            op[x,y]=(max(0,min(255,int(r*255))),max(0,min(255,int(g*255))),max(0,min(255,int(b*255))))
else:
    sys.exit(f"unhandled dxgiFormat {fmt}")

img.save(out)
print("wrote", out)
