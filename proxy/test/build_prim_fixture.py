"""Convert the per-primitive test mesh shader with ms2cs.py and embed the pair as g_primCS and g_primVS."""
from pathlib import Path
import subprocess
import sys
import tempfile

tools = Path(__file__).resolve().parents[2] / 'tools'
source, target = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory() as out:
    subprocess.run([sys.executable, str(tools / 'ms2cs.py'), out, str(source)], check=True)
    blobs = {ext: next(Path(out).glob('*.emu%s7.cso' % ext)).read_bytes() for ext in ('cs', 'vs')}
target.write_text(''.join('static const unsigned char g_prim%s[] = {\n%s\n};\n' % (
    ext.upper(), ',\n'.join(','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)))
    for ext, data in blobs.items()))
