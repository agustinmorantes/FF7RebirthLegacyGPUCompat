"""Grep ASCII strings in the tail region (index area) of a pak. usage: pakgrep.py file.pak regex [tail_bytes]"""
import sys, re
p, pat = sys.argv[1], re.compile(sys.argv[2].encode())
n = int(sys.argv[3]) if len(sys.argv) > 3 else 4 << 20
with open(p, 'rb') as f:
    f.seek(0, 2); size = f.tell(); f.seek(max(0, size - n)); d = f.read()
for m in re.finditer(rb'[\x20-\x7e]{4,}', d):
    if pat.search(m.group()): print(m.group().decode())
