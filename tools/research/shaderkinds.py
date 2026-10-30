"""Decompress (LZ4) every shader in a UE shader archive inside a pak, read the DXIL shader kind.
usage: shaderkinds.py pak entry_offset [--dump outdir kind ...]"""
import sys, struct, collections, lz4.block, os
pak, eoff = sys.argv[1], int(sys.argv[2])
dump = sys.argv[sys.argv.index('--dump') + 1:] if '--dump' in sys.argv else None
f = open(pak, 'rb'); base = eoff + 53; f.seek(base); blob = f.read(64 << 20); o = 0
def u(fmt):
    global o; v = struct.unpack_from('<' + fmt, blob, o); o += struct.calcsize('<' + fmt); return v if len(v) > 1 else v[0]
u('I'); n = u('i'); o += 20 * n; n = u('i'); hashes = [blob[o + 20*i:o + 20*i + 20].hex() for i in range(n)]; o += 20 * n
n = u('i'); o += 16 * n
n = u('i'); entries = []
for _ in range(n): entries.append(struct.unpack_from('<QIIB', blob, o)); o += 17
n = u('i'); o += 16 * n; n = u('i'); o += 4 * n; code_base = base + o
KIND = {0: 'PS', 1: 'VS', 2: 'GS', 3: 'HS', 4: 'DS', 5: 'CS', 6: 'Lib', 13: 'MS', 14: 'AS'}
hist = collections.Counter(); sizes = collections.defaultdict(int)
for idx, (off, sz, usz, fr) in enumerate(entries):
    f.seek(code_base + off); raw = f.read(sz)
    d = lz4.block.decompress(raw, uncompressed_size=usz) if sz != usz else raw
    p = d.find(b'DXBC')
    kind = 'noDXBC'
    if p >= 0:
        nparts = struct.unpack_from('<I', d, p + 28)[0]
        for k in range(nparts):
            po = p + struct.unpack_from('<I', d, p + 32 + 4 * k)[0]
            if d[po:po + 4] == b'DXIL':
                pv = struct.unpack_from('<I', d, po + 8)[0]; kind = KIND.get(pv >> 16, 'k%d' % (pv >> 16))
                kind += '_%d.%d' % ((pv >> 4) & 0xf, pv & 0xf)
                break
    hist[(fr, kind)] += 1; sizes[(fr, kind)] += usz
    if dump and kind.split('_')[0] in dump[1:]:
        os.makedirs(dump[0], exist_ok=True)
        open(os.path.join(dump[0], '%s_%05d_%s.bin' % (kind, idx, hashes[idx][:12])), 'wb').write(d[p:])
for k, v in sorted(hist.items()): print("UEfreq=%d kind=%-8s count=%6d  bytes=%d" % (k[0], k[1], v, sizes[k]))
