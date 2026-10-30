"""Runs proxytest in a temporary copy of the build for each config the suite covers.

The tests expect a fixed 256 MiB scratch, so each run gets its own legacygpu.ini.
usage: run_tests.py [build dir]   (default: proxy/build)
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / 'build'
# MeshPerformanceDiagnostics, PassPerformanceDiagnostics
CASES = [(0, 0), (1, 0), (2, 0), (0, 1)]
failed = 0
for mode, passes in CASES:
    settings = {'MeshPerformanceDiagnostics': mode, 'PassPerformanceDiagnostics': passes, 'MeshScratchMinMiB': 256,
                'MeshScratchMaxMiB': 256}
    with tempfile.TemporaryDirectory() as tmp:
        run = Path(tmp)
        for name in ('d3d12.dll', 'proxytest.exe'):
            shutil.copy2(build / name, run / name)
        shutil.copytree(build / 'legacygpu_shaders', run / 'legacygpu_shaders')
        ini = (build / 'legacygpu.ini').read_text()
        for key, value in settings.items():
            ini, count = re.subn(rf'(?m)^{key}=\d+', f'{key}={value}', ini)
            assert count == 1, key
        (run / 'legacygpu.ini').write_text(ini)
        result = subprocess.run([str(run / 'proxytest.exe')], cwd=run, capture_output=True, text=True, timeout=600)
        out = result.stdout + result.stderr
        ok = result.returncode == 0 and re.search(r'^OK \(0 failures\)', out, re.M)
        print(f'{mode}:{passes} {"OK" if ok else "FAILED"} ({len(re.findall(r"^PASS", out, re.M))} passed)')
        if not ok:
            failed += 1
            print('\n'.join(line for line in out.splitlines() if line.startswith('FAIL')))
            print((run / 'legacygpu.log').read_text(errors='replace')[-4000:])
sys.exit(1 if failed else 0)
