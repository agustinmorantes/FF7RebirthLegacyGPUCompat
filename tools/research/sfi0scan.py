"""Histogram DXIL SFI0 shader feature flags across a UE shader archive inside a pak. usage: sfi0scan.py pak entry_offset"""
import sys, struct, collections, lz4.block
pak, eoff = sys.argv[1], int(sys.argv[2])
f = open(pak, 'rb'); base = eoff + 53; f.seek(base); blob = f.read(64 << 20); o = 0
def u(fmt):
    global o; v = struct.unpack_from('<' + fmt, blob, o); o += struct.calcsize('<' + fmt); return v if len(v) > 1 else v[0]
u('I'); n = u('i'); o += 20 * n; n = u('i'); o += 20 * n; n = u('i'); o += 16 * n
n = u('i'); entries = []
for _ in range(n): entries.append(struct.unpack_from('<QIIB', blob, o)); o += 17
n = u('i'); o += 16 * n; n = u('i'); o += 4 * n; code_base = base + o
NAMES = {0x1:'Doubles',0x4:'UAVsEveryStage',0x8:'64UAVs',0x10:'MinPrecision',0x100:'TiledResources',0x200:'StencilRef',0x400:'InnerCoverage',
 0x800:'TypedUAVLoadAddlFormats',0x1000:'ROVs',0x2000:'VPRTIndexFromAnyStage',0x4000:'WaveOps',0x8000:'Int64Ops',0x10000:'ViewID',0x20000:'Barycentrics',
 0x40000:'NativeLowPrecision',0x80000:'ShadingRate',0x100000:'DXR1.1',0x200000:'SamplerFeedback',0x400000:'AtomicInt64Typed',0x800000:'AtomicInt64Groupshared',
 0x1000000:'DerivInMS/AS',0x2000000:'ResDescHeapIndexing',0x4000000:'SamplerHeapIndexing',0x10000000:'AtomicInt64Heap',0x20000000:'AdvancedTextureOps',
 0x40000000:'WriteableMSAA',0x80000000:'SampleCmpGradBias',0x100000000:'ExtendedCommandInfo'}
KIND = {0:'PS',1:'VS',2:'GS',3:'HS',4:'DS',5:'CS',6:'Lib',13:'MS',14:'AS'}
hist = collections.defaultdict(collections.Counter)
for off, sz, usz, fr in entries:
    f.seek(code_base + off); raw = f.read(sz); d = lz4.block.decompress(raw, uncompressed_size=usz) if sz != usz else raw
    p = d.find(b'DXBC'); flags = 0; kind = '?'
    for k in range(struct.unpack_from('<I', d, p + 28)[0]):
        po = p + struct.unpack_from('<I', d, p + 32 + 4 * k)[0]
        if d[po:po+4] == b'SFI0': flags = struct.unpack_from('<Q', d, po + 8)[0]
        if d[po:po+4] == b'DXIL': kind = KIND.get(struct.unpack_from('<I', d, po + 8)[0] >> 16, '?')
    hist[kind]['_total'] += 1
    for bit, nm in NAMES.items():
        if flags & bit: hist[kind][nm] += 1
    for bit in range(40):
        if flags & (1 << bit) and (1 << bit) not in NAMES: hist[kind]['bit%d' % bit] += 1
for k, c in hist.items(): print(k, dict(sorted(c.items())))
