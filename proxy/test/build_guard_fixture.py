"""Build a real converted mesh shader; the test gives it room for only one meshlet's indices."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ms2cs

source, target = map(Path, sys.argv[1:])
dxc = str(Path(ms2cs.BIN) / 'dxc.exe')
ll = subprocess.run([dxc, '-dumpbin', str(source)], capture_output=True, text=True, check=True).stdout
half = ms2cs.half_pairs(ll, ms2cs.entry_info(ll)['outputs'])
ll = ms2cs.fix16.fix(ll)
info = ms2cs.entry_info(ll)
info['half'] = half
ir = target.with_suffix('.ll')
binary = target.with_suffix('.cso')
ir.write_text(ms2cs.convert_cs(ll, info))
subprocess.run([str(Path(ms2cs.BIN) / 'dxilasm.exe'), str(ir), str(binary)], check=True)
data = binary.read_bytes()
target.write_text('static const unsigned char g_guardCS[] = {\n' +
                  ',\n'.join(','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)) + '\n};\n')
