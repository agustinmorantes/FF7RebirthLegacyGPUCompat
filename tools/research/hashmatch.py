# Matches DXIL container digests (as logged by the proxy) against dumped shader files.
# usage: hashmatch.py log dirs...
import sys, os, re, csv
want = set(re.findall(r'(?:MS|CS|PS|VS)=([0-9a-f]{32})', open(sys.argv[1], errors='replace').read()))
names = {}
if os.path.exists('work/ms_analysis.csv'):
    for r in csv.DictReader(open('work/ms_analysis.csv')): names[r['file']] = r
for d in sys.argv[2:]:
    for f in os.listdir(d):
        b = open(os.path.join(d, f), 'rb').read(64)
        i = b.find(b'DXBC')
        if i < 0: continue
        h = b[i+4:i+20].hex()
        if h in want:
            r = names.get(f, {})
            print(h, d, f, r.get('entry', ''), r.get('threads', ''), 'gs=' + r.get('groupshared', ''), r.get('pout', ''))
