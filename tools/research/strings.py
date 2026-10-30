"""Extract ASCII and UTF-16LE strings (like `strings` / `strings -e l`) from a binary."""
import re, sys
path, out, n = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 6
data = open(path, 'rb').read()
with open(out, 'w', encoding='utf-8') as f:
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, data):
        f.write('A %08x %s\n' % (m.start(), m.group().decode('ascii')))
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % n, data):
        f.write('W %08x %s\n' % (m.start(), m.group().decode('utf-16le')))
