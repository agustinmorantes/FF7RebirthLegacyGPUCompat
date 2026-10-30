"""Packages a release zip: the proxy, its ini and the tools that convert the game's shaders.

The zip mirrors the game folder (End/Binaries/Win64/...), so players extract it into the folder
that contains End. It holds no game files: on the first start and after each game update, the
proxy runs the bundled build_shaders.py with the bundled Python on the installed game.
usage: package_release.py version   (run proxy\\build.cmd first)
"""
import hashlib
import io
from pathlib import Path
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
WIN64 = 'End/Binaries/Win64/'
TOOLS = WIN64 + 'legacygpu_tools/'
DXC = 'dxc-1.9.2609/'
PYTHON = ('https://www.python.org/ftp/python/3.14.9/python-3.14.9-embed-amd64.zip',
          '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2')
LZ4 = ('https://files.pythonhosted.org/packages/18/2c/8332080fd293f8337779a440b3a143f85e374311705d243439a3349b81ad/'
       'lz4-4.4.5-cp314-cp314-win_amd64.whl', '92159782a4502858a21e0079d77cdcaade23e8a5d252ddf46b0652604300d7be')
README = """Legacy GPU Compatibility {version} for FINAL FANTASY VII REBIRTH

Install: close the game and remove any other mod's d3d12.dll from End\\Binaries\\Win64 first.
Extract the entire zip into the game folder (the one that contains End), merging the folders,
so that d3d12.dll, legacygpu.ini and legacygpu_tools are directly in End\\Binaries\\Win64.
Do not extract into Win64 or leave the files inside an extra zip-named folder. No separate
Python or developer tools installation is needed. On the first start, and after each game
update, the mod converts the game's shaders before the game starts. That takes a few minutes;
a progress window appears after six seconds. Then let the game's own shader compilation finish.

Uninstall: delete d3d12.dll, legacygpu.ini, legacygpu.log, legacygpu_README.txt,
legacygpu_LICENSE.txt and the legacygpu_tools and legacygpu_shaders folders from
End\\Binaries\\Win64.

The legacygpu_tools folder contains Python {python} (python/LICENSE.txt), python-lz4
(python/lz4-LICENSE.txt) and the DirectX Shader Compiler (license files in dxc-1.9.2609).

Source, documentation and issues: https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat

Acknowledgements: frostbone25 and the ShaderInjector contributors. ShaderInjector served as a
reference for hooking D3D12 shader creation: https://github.com/frostbone25/ShaderInjector
Optional additions: ShaderInjector for visual adjustments; OptiScaler for upscaling and XeFG
frame generation where supported. Setup links and compatibility notes are in the project README.
"""


def download(url, sha256):
    """Fetches url once into tools/downloads and checks it against the pinned hash."""
    cache = ROOT / 'tools/downloads' / url.rsplit('/', 1)[1]
    if not cache.exists():
        cache.parent.mkdir(exist_ok=True)
        with urllib.request.urlopen(url) as r:
            cache.write_bytes(r.read())
    data = cache.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha256:
        cache.unlink()
        sys.exit(f'{url}: SHA-256 mismatch')
    return zipfile.ZipFile(io.BytesIO(data))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    version = sys.argv[1]
    dll = ROOT / 'proxy/build/d3d12.dll'
    dxc = ROOT / 'tools' / DXC
    if not dll.exists() or not (dxc / 'bin/x64/dxilasm.exe').exists():
        sys.exit('run proxy\\build.cmd first')
    python, lz4 = download(*PYTHON), download(*LZ4)
    out = ROOT / 'dist' / f'LegacyGPUCompat-{version}.zip'
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(dll, WIN64 + 'd3d12.dll')
        z.write(ROOT / 'proxy/legacygpu.ini', WIN64 + 'legacygpu.ini')
        z.write(ROOT / 'LICENSE', WIN64 + 'legacygpu_LICENSE.txt')
        readme = README.format(version=version, python=PYTHON[0].split('/')[-2])
        z.writestr(WIN64 + 'legacygpu_README.txt', readme.replace('\n', '\r\n'))
        for name in ('build_shaders.py', 'ms2cs.py', 'fix16.py'):
            z.write(ROOT / 'tools' / name, TOOLS + name)
        for name in ('dxc.exe', 'dxcompiler.dll', 'dxil.dll', 'dxilasm.exe'):
            z.write(dxc / 'bin/x64' / name, TOOLS + DXC + 'bin/x64/' + name)
        for path in dxc.glob('LICEN*.txt'):
            z.write(path, TOOLS + DXC + path.name)
        for info in python.infolist():
            z.writestr(TOOLS + 'python/' + info.filename, python.read(info))
        for info in lz4.infolist():
            if info.filename.startswith('lz4/'):
                z.writestr(TOOLS + 'python/' + info.filename, lz4.read(info))
            elif info.filename.endswith('/licenses/LICENSE'):
                z.writestr(TOOLS + 'python/lz4-LICENSE.txt', lz4.read(info))
    print(f'{out} ({out.stat().st_size / 2**20:.1f} MiB)')
    checksum = out.with_suffix('.zip.sha256')
    checksum.write_text(f'{hashlib.sha256(out.read_bytes()).hexdigest()}  {out.name}\n', encoding='ascii')


if __name__ == '__main__':
    main()
