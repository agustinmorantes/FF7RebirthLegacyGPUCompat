"""Bulk-analyze DXIL mesh shaders with dxc -dumpbin. usage: msanalyze.py dxc out.csv dir [dir...]"""
import sys, os, re, csv, subprocess, collections
from concurrent.futures import ThreadPoolExecutor
dxc, out, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
files = [os.path.join(d, f) for d in dirs for f in sorted(os.listdir(d))]
def section(txt, title):
    m = re.search(r'; %s:\n;\n; Name.*\n; -+.*\n((?:; \S.*\n)*)' % re.escape(title), txt)
    return [l[2:].split()[0] for l in m.group(1).splitlines()] if m else []
def analyze(path):
    t = subprocess.run([dxc, '-dumpbin', path], capture_output=True, text=True, errors='replace').stdout
    g = lambda pat, d='': (re.search(pat, t) or [None, d])[1]
    entry = g(r'EntryFunctionName: (\S+)')
    tag = re.search(r'i32 9, !(\d+)', g(r'(!\d+ = !\{i32 \d+, i64 \d+, i32 9, .*)', ''))
    maxv = maxp = payload = ''
    if tag:
        m = re.search(r'^!%s = !\{!\d+, i32 (\d+), i32 (\d+), i32 \d+, i32 (\d+)\}' % tag.group(1), t, re.M)
        if m: maxv, maxp, payload = m.groups()
    ops = collections.Counter(re.findall(r'call [^@]*@dx\.op\.([a-zA-Z0-9]+)', t))
    return dict(file=os.path.basename(path), dir=os.path.basename(os.path.dirname(path)), entry=entry,
        threads=g(r'NumThreads=\(([^)]*)\)').replace(',', 'x'), maxV=maxv, maxP=maxp, payload=payload,
        vout=' '.join(section(t, 'Vertex Output signature') or section(t, 'Output signature')), pout=' '.join(section(t, 'Primitive Output signature')),
        groupshared=int(bool(re.search(r'addrspace\(3\)', t))), barriers=ops['barrier'], waveops=sum(v for k, v in ops.items() if k.startswith('wave') or k.startswith('quad')),
        setcounts=ops['setMeshOutputCounts'], viewid=g(r'UsesViewID: (\w+)'), lines=t.count('\n'),
        dynres=int('createHandleFromHeap' in t), atomics=sum(v for k, v in ops.items() if 'tomic' in k))
with ThreadPoolExecutor(12) as ex: rows = list(ex.map(analyze, files))
with open(out, 'w', newline='') as fh:
    w = csv.DictWriter(fh, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
print("analyzed", len(rows))
