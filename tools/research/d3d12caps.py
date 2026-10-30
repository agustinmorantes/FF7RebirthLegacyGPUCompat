"""Query D3D12 feature support of the default adapter via raw COM vtable calls."""
import ctypes as C
from ctypes import wintypes as W

class GUID(C.Structure):
    _fields_ = [("a", C.c_uint32), ("b", C.c_uint16), ("c", C.c_uint16), ("d", C.c_ubyte * 8)]
def guid(s):
    p = s.split('-'); g = GUID(int(p[0], 16), int(p[1], 16), int(p[2], 16))
    for i, x in enumerate(bytes.fromhex(p[3] + p[4])): g.d[i] = x
    return g

d3d12 = C.WinDLL("d3d12.dll")
dev = C.c_void_p()
hr = d3d12.D3D12CreateDevice(None, 0xb000, C.byref(guid("189819f1-1db6-4b57-be54-1821339b85f7")), C.byref(dev))
assert hr == 0, hex(hr & 0xffffffff)
vtbl = C.cast(C.cast(dev, C.POINTER(C.c_void_p))[0], C.POINTER(C.c_void_p))
CFS = C.WINFUNCTYPE(C.c_long, C.c_void_p, C.c_int, C.c_void_p, C.c_uint)(vtbl[13])

def q(feat, n, init=None):
    buf = (C.c_int32 * n)(*(init or []))
    hr = CFS(dev, feat, buf, C.sizeof(buf))
    return list(buf) if hr == 0 else "hr=" + hex(hr & 0xffffffff)

for sm in (0x69, 0x68, 0x67, 0x66, 0x65):
    r = q(7, 1, [sm])
    if isinstance(r, list): print("HighestShaderModel: %x" % r[0]); break
o = q(0, 15); print("OPTIONS: TiledResourcesTier=%d ResourceBindingTier=%d ConservativeRaster=%d ResourceHeapTier=%d ROVs=%d" % (o[3], o[4], o[8], o[14], o[7]))
o = q(8, 6); print("OPTIONS1: WaveOps=%d LaneMin=%d LaneMax=%d Int64Ops=%d" % (o[0], o[1], o[2], o[5]))
o = q(21, 5); print("OPTIONS3: ViewInstancingTier=%d Barycentrics=%d" % (o[3], o[4]))
o = q(27, 3); print("OPTIONS5: SRVOnlyTiledTier3=%d RenderPassesTier=%d RaytracingTier=%d" % tuple(o))
o = q(30, 5); print("OPTIONS6: VRS tier=%d" % o[2])
o = q(32, 2); print("OPTIONS7: MeshShaderTier=%d SamplerFeedbackTier=%d" % tuple(o) if isinstance(o, list) else "OPTIONS7: " + o)
o = q(37, 6); print("OPTIONS9:", o, "(MSPipelineStats, MSFullRangeRTArrayIdx, AtomicInt64Typed, AtomicInt64Groupshared, DerivativesInMS/AS, WaveMMATier)")
o = q(40, 1); print("OPTIONS11: AtomicInt64OnDescriptorHeapResource:", o)
fl = (C.c_int32 * 6)(5, 0xb000, 0xb100, 0xc000, 0xc100, 0xc200); hr = CFS(dev, 1, None, 0)
buf = C.create_string_buffer(24)
import struct
levels = (C.c_int32 * 5)(0xb000, 0xb100, 0xc000, 0xc100, 0xc200)
class FL(C.Structure): _fields_ = [("n", C.c_uint), ("p", C.c_void_p), ("max", C.c_int)]
s = FL(5, C.cast(levels, C.c_void_p), 0)
print("MaxFeatureLevel: %x" % s.max if CFS(dev, 2, C.byref(s), C.sizeof(s)) == 0 else "FL query failed", hex(s.max))
o = q(23, 3); print("OPTIONS4: MSAA64KB=%d SharedResTier=%d Native16BitShaderOps=%d" % tuple(o))
o = q(42, 3) if True else None; print("OPTIONS12 (EnhancedBarriers etc.):", o)
o = q(43, 1); print("OPTIONS13:", q(43, 4))
o = q(45, 3); print("OPTIONS14 (AdvancedTextureOps, WriteableMSAA, IndependentFrontAndBackStencilRef):", o)
