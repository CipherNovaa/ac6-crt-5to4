#!/usr/bin/env python3
# Disassemble the byte windows dumped by the aspect-clamp hunter (ac6asp.bin).
# Each record: [u64 window_start_module_offset][u32 len][len bytes].
# Module base = 0x140000000. Highlights MAXSS/MINSS/COMISS/UCOMISS (the clamp ops).
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

BASE = 0x140000000
path = sys.argv[1] if len(sys.argv) > 1 else "ac6asp.bin"
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False

CLAMP = ("maxss", "minss", "comiss", "ucomiss", "maxps", "minps")
data = open(path, "rb").read()
off = 0
rec = 0
while off + 12 <= len(data):
    woff, ln = struct.unpack_from("<QI", data, off); off += 12
    blob = data[off:off+ln]; off += ln
    addr = BASE + woff
    print("\n========== window @ +0x%x (va 0x%x) ==========" % (woff, addr))
    for ins in md.disasm(blob, addr):
        mark = ""
        m = ins.mnemonic
        if m in CLAMP: mark = "   <<<<< CLAMP OP"
        elif m.startswith("j") and m != "jmp": mark = "   <- branch"
        print("  0x%x:  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))
    rec += 1
print("\n%d windows" % rec)
