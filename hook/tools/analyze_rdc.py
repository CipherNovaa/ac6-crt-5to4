import renderdoc as rd
import os, sys, struct, math

_log = open(os.environ.get("RDC_OUT", "rdc_result.txt"), "w", buffering=1)
def out(*a):
    _log.write(" ".join(str(x) for x in a) + "\n"); _log.flush()

if len(sys.argv) < 2:
    out("usage: qrenderdoc --python analyze_rdc.py <capture.rdc>"); os._exit(2)
path = sys.argv[1]
cap = rd.OpenCaptureFile()
if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
    out("OPEN FAILED"); import os; os._exit(1)
r = cap.OpenCapture(rd.ReplayOptions(), None)
ctrl = r[1] if isinstance(r, tuple) else r
out("replay opened:", path)

def walk(acts):
    for a in acts:
        yield a
        for c in a.children:
            for x in walk([c]): yield x

draws = [a for a in walk(ctrl.GetRootActions()) if (a.flags & rd.ActionFlags.Drawcall)]
draws.sort(key=lambda a: a.numIndices, reverse=True)
out("total draws:", len(draws), "| top index counts:", [d.numIndices for d in draws[:5]])

def scan_matrix(data):
    n = len(data)//4
    if n < 16: return []
    f = struct.unpack("<%df"%n, bytes(data[:n*4]))
    hits = []
    for i in range(0, n-15):
        if abs(f[i+15]) > 0.001: continue
        a, b = f[i+11], f[i+14]
        if abs(abs(a)-1) > 0.06 and abs(abs(b)-1) > 0.06: continue
        cx = math.sqrt(f[i]**2+f[i+4]**2+f[i+8]**2+f[i+12]**2)
        cy = math.sqrt(f[i+1]**2+f[i+5]**2+f[i+9]**2+f[i+13]**2)
        if cy < 1e-6 or cx < 1e-6: continue
        ratio = cx/cy
        if 0.1 < ratio < 3.0:
            hits.append((i*4, round(ratio,4)))
    return hits

def bound_cb(pipe, stage, idx):
    bc = pipe.GetConstantBuffer(stage, idx, 0)
    # handle both old (.resourceId/.byteOffset/.byteSize) and new (.descriptor.*) APIs
    rid = getattr(bc, "resourceId", None)
    off = getattr(bc, "byteOffset", None)
    sz  = getattr(bc, "byteSize", None)
    if rid is None and hasattr(bc, "descriptor"):
        d = bc.descriptor; rid = getattr(d, "resource", None); off = getattr(d, "byteOffset", 0); sz = getattr(d, "byteSize", 0)
    return rid, (off or 0), (sz or 0)

seen = set()
for a in draws[:6]:
    ctrl.SetFrameEvent(a.eventId, True)
    pipe = ctrl.GetPipelineState()
    out("=== draw eid", a.eventId, "numIndices", a.numIndices, "===")
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
        try: refl = pipe.GetShaderReflection(stage)
        except Exception as e: refl = None
        if not refl: continue
        for idx, cb in enumerate(refl.constantBlocks):
            try:
                rid, off, sz = bound_cb(pipe, stage, idx)
            except Exception as e:
                continue
            if rid is None or int(rid) == 0: continue
            key = (int(rid), off, sz)
            if key in seen: continue
            seen.add(key)
            try:
                data = ctrl.GetBufferData(rid, off, sz if sz else 0)
            except Exception as e:
                continue
            for moff, ratio in scan_matrix(data):
                tag = "  <<< 16:9 CAMERA" if abs(ratio-0.5625) < 0.04 else ""
                out("  [%s] %s slot%d cbName=%s  matrix@+%d ratio=%.4f%s" %
                    (str(stage), "VS" if stage==rd.ShaderStage.Vertex else "PS", idx, getattr(cb,"name",""), moff, ratio, tag))

out("DONE")
ctrl.Shutdown(); cap.Shutdown()
import os
os._exit(0)
