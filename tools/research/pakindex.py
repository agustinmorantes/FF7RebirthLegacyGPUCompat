"""Minimal UE4 pak v11 index reader (unencrypted). usage: pakindex.py file.pak [regex] [--extract outdir]"""
import sys, struct, re, os, zlib
p = sys.argv[1]; pat = re.compile(sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else '.')
out = sys.argv[sys.argv.index('--extract') + 1] if '--extract' in sys.argv else None
f = open(p, 'rb'); f.seek(0, 2); size = f.tell(); f.seek(size - 512); tail = f.read()
i = tail.rfind(struct.pack('<I', 0x5A6F12E1)); ver, idxoff, idxsize = struct.unpack_from('<IQQ', tail, i + 4)
comp = tail[i + 52:]; methods = [comp[j:j+32].rstrip(b'\0').decode() for j in range(0, 160, 32)]
class R:
    def __init__(s, b): s.b, s.o = b, 0
    def u(s, fmt): v = struct.unpack_from('<' + fmt, s.b, s.o); s.o += struct.calcsize('<' + fmt); return v if len(v) > 1 else v[0]
    def fstr(s):
        n = s.u('i')
        if n == 0: return ''
        if n < 0: v = s.b[s.o:s.o - 2 * n].decode('utf-16le'); s.o -= 2 * n
        else: v = s.b[s.o:s.o + n].decode('latin1'); s.o += n
        return v.rstrip('\0')
f.seek(idxoff); r = R(f.read(idxsize))
mount = r.fstr(); nent = r.u('i'); seed = r.u('Q')
if r.u('i'): r.u('qq'); r.o += 20
hasfdi = r.u('i'); fdioff, fdisize = r.u('qq'); r.o += 20
enc = r.b[r.o + 4: r.o + 4 + r.u('i')]
f.seek(fdioff); d = R(f.read(fdisize))
files = []
for _ in range(d.u('i')):
    dn = d.fstr()
    for _ in range(d.u('i')): files.append((dn + d.fstr(), d.u('i')))
def decode(off):
    e = R(enc); e.o = off; v = e.u('I')
    bsz = (v & 0x3f) << 11
    if (v & 0x3f) == 0x3f: bsz = e.u('I')
    nblk = (v >> 6) & 0xffff; encd = (v >> 22) & 1; cm = (v >> 23) & 0x3f
    o = e.u('I') if v >> 31 & 1 else e.u('Q'); usz = e.u('I') if v >> 30 & 1 else e.u('Q')
    csz = (e.u('I') if v >> 29 & 1 else e.u('Q')) if cm else usz
    blocks = [e.u('I') for _ in range(nblk)] if (nblk > 1 or (nblk == 1 and encd)) else ([csz] if nblk else [])
    return dict(offset=o, usize=usz, csize=csz, method=methods[cm - 1] if cm else 'none', nblocks=nblk, bsize=bsz, encrypted=encd, blocks=blocks)
print(f"mount={mount} entries={nent} files={len(files)}")
for name, off in files:
    if not pat.search(name): continue
    e = decode(off); print(name, {k: v for k, v in e.items() if k != 'blocks'})
    if out:
        assert not e['encrypted'] and e['method'] in ('none', 'Zlib'), "cannot extract " + e['method']
        hdr = 8 + 8 + 8 + 4 + 20 + (4 + 16 * e['nblocks'] if e['method'] != 'none' else 0) + 1 + 4  # v11 FPakEntry header
        pos = e['offset'] + hdr; buf = bytearray()
        f.seek(pos)
        if e['method'] == 'none': buf = f.read(e['usize'])
        else:
            for b in e['blocks']: buf += zlib.decompress(f.read(b))
        os.makedirs(out, exist_ok=True); open(os.path.join(out, os.path.basename(name)), 'wb').write(buf)
        print("  extracted", len(buf))
