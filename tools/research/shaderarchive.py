"""Parse a UE4.26 FSerializedShaderArchive (.ushaderbytecode) in place inside a pak and histogram shaders.
usage: shaderarchive.py pak entry_offset entry_size"""
import sys, struct, collections
pak, eoff, esize = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
f = open(pak, 'rb'); hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4
base = eoff + hdr; f.seek(base)
blob = f.read(min(esize, 64 << 20))
o = 0
def u(fmt):
    global o; v = struct.unpack_from('<' + fmt, blob, o); o += struct.calcsize('<' + fmt); return v if len(v) > 1 else v[0]
ver = u('I'); print("archive version", ver)
n = u('i'); o += 20 * n; print("shadermaps", n)
nsh = u('i'); o += 20 * nsh; print("shaders", nsh)
n = u('i'); o += 16 * n
n = u('i'); entries = []
for _ in range(n):
    off, sz, usz, freq = struct.unpack_from('<QIIB', blob, o); o += 17; entries.append((off, sz, usz, freq))
n = u('i'); o += 16 * n
n = u('i'); o += 4 * n
code_base = base + o
print("code starts at", code_base, "entries", len(entries))
freqs = collections.Counter(e[3] for e in entries)
print("frequency histogram (UE enum):", sorted(freqs.items()))
comp = sum(1 for e in entries if e[1] != e[2]); print("compressed entries:", comp)
# sample first bytes of a few compressed/uncompressed entries per frequency
seen = set()
for off, sz, usz, fr in entries:
    if fr in seen: continue
    seen.add(fr); f.seek(code_base + off); d = f.read(min(sz, 64))
    print("freq", fr, "size", sz, "usize", usz, "head", d[:16].hex())
