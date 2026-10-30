"""Compile a real multidimensional mesh fixture through the production v4 converter."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ms2cs

source, target = map(Path, sys.argv[1:])
dxc = str(Path(ms2cs.BIN) / 'dxc.exe')
stem = target.with_suffix('')
ms = str(stem) + '.ms.cso'
subprocess.run([dxc, '-T', 'ms_6_6', '-E', 'MSMain', '-Fo', ms, str(source)], check=True)
ll = subprocess.run([dxc, '-dumpbin', ms], capture_output=True, text=True, check=True).stdout
half = ms2cs.half_pairs(ll, ms2cs.entry_info(ll)['outputs'])
assert len(half) == 1, half
ll = ms2cs.fix16.fix(ll)
info = ms2cs.entry_info(ll)
info['half'] = half
ir = Path(str(stem) + '.cs.ll')
ir.write_text(ms2cs.convert_cs(ll, info))
cs, vs, ps = [str(stem) + '.' + kind + '.cso' for kind in ('cs', 'vs', 'ps')]
subprocess.run([str(Path(ms2cs.BIN) / 'dxilasm.exe'), str(ir), cs], check=True)
vs_source = Path(str(stem) + '.vs.hlsl')
vs_source.write_text(ms2cs.vs_source(info['outputs'], half=info['half']))
subprocess.run([dxc, '-T', 'vs_6_6', '-Fo', vs, str(vs_source)], check=True)
subprocess.run([dxc, '-T', 'ps_6_6', '-E', 'PSMain', '-Fo', ps, str(source)], check=True)
raw_ms = str(stem) + '.rawms.cso'
subprocess.run([dxc, '-T', 'ms_6_6', '-E', 'MSBits', '-Fo', raw_ms, str(source)], check=True)
raw_ll = subprocess.run([dxc, '-dumpbin', raw_ms], capture_output=True, text=True, check=True).stdout
raw_info = ms2cs.entry_info(raw_ll)
assert not ms2cs.half_pairs(raw_ll, raw_info['outputs'])
raw_cs = []
for vector in (False, True):
    ir.write_text(ms2cs.convert_cs(raw_ll, raw_info, vector_stores=vector))
    output = str(stem) + f'.raw{int(vector)}.cso'
    subprocess.run([str(Path(ms2cs.BIN) / 'dxilasm.exe'), str(ir), output], check=True)
    raw_cs.append(Path(output).read_bytes())
target.write_text(''.join('static const unsigned char g_retry%s[] = {\n%s\n};\n' % (
    kind, ',\n'.join(','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)))
    for kind, data in [('CS', Path(cs).read_bytes()), ('VS', Path(vs).read_bytes()), ('PS', Path(ps).read_bytes()),
                       ('RawScalarCS', raw_cs[0]), ('RawCS', raw_cs[1])]))
