"""Builds the proxy's shader library from an installed copy of the game.

Reads the SM6 shader archives in pakchunk0, converts every mesh shader with ms2cs.py and every
compute shader that needs native 16-bit ops with fix16.py, and writes the results to outdir
(normally <game>/End/Binaries/Win64/legacygpu_shaders). Shaders the converters reject are listed
and skipped; the proxy keeps skipping their draws.

Incremental: shaders whose outputs are already in outdir are kept, outputs of shaders the game no
longer has are deleted, and everything is rebuilt when the converters change (converter.txt).
The proxy runs this itself after a game update (proxy/src/generate.cpp).
usage: build_shaders.py "<game dir>" outdir
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import lz4.block

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ms2cs  # noqa: E402

PAK = Path('End/Content/Paks/pakchunk0-WindowsNoEditor.pak')
ARCHIVES = ('End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode',
            'End/Content/ShaderArchive-End-PCD3D_SM6.ushaderbytecode')
KIND_CS, KIND_MS = 5, 13
NATIVE_LOW_PRECISION = 0x40000  # SFI0 feature flag


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u(self, fmt):
        v = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]

    def fstr(self):
        n = self.u('i')
        if n < 0:
            v = self.data[self.pos:self.pos - 2 * n].decode('utf-16le')
            self.pos -= 2 * n
        else:
            v = self.data[self.pos:self.pos + n].decode('latin1')
            self.pos += n
        return v.rstrip('\0')

    def skip_array(self, item_size):
        n = self.u('i')
        self.pos += item_size * n


def pak_entries(f):
    """{path: data offset} of the uncompressed, unencrypted entries of a v11 pak."""
    f.seek(0, 2)
    f.seek(f.tell() - 512)
    tail = f.read()
    i = tail.rfind(struct.pack('<I', 0x5A6F12E1))
    version, index_offset, index_size = struct.unpack_from('<IQQ', tail, i + 4)
    assert version == 11 and not tail[i - 1], 'expected an unencrypted v11 pak index'
    f.seek(index_offset)
    r = Reader(f.read(index_size))
    r.fstr()
    r.u('iQ')
    if r.u('i'):
        r.pos += 36
    r.u('i')
    fdi_offset, fdi_size = r.u('qq')
    r.pos += 20
    n = r.u('i')
    encoded = r.data[r.pos:r.pos + n]
    f.seek(fdi_offset)
    d = Reader(f.read(fdi_size))
    entries = {}
    for _ in range(d.u('i')):
        directory = d.fstr()
        for _ in range(d.u('i')):
            name, at = directory + d.fstr(), d.u('i')
            if at < 0:
                continue
            e = Reader(encoded)
            e.pos = at
            bits = e.u('I')
            if (bits & 0x3f) == 0x3f:
                e.u('I')
            offset = e.u('I') if bits >> 31 & 1 else e.u('Q')
            if not (bits >> 23) & 0x3f and not (bits >> 22) & 1:  # stored, not encrypted
                entries[name] = offset + 53  # v11 FPakEntry header
    return entries


def shaders(f, base):
    """Yields every DXBC container in a UE4.26 FSerializedShaderArchive at file offset base."""
    f.seek(base)
    r = Reader(f.read(64 << 20))
    r.u('I')
    r.skip_array(20)  # shader map hashes
    r.skip_array(20)  # shader hashes
    r.skip_array(16)  # shader map entries
    entries = [r.u('QIIB') for _ in range(r.u('i'))]
    r.skip_array(16)  # preload entries
    r.skip_array(4)  # shader indices
    code = base + r.pos
    for offset, size, usize, _ in entries:
        f.seek(code + offset)
        data = f.read(size)
        if size != usize:
            data = lz4.block.decompress(data, uncompressed_size=usize)
        p = data.find(b'DXBC')
        if p >= 0:
            yield data[p:p + struct.unpack_from('<I', data, p + 24)[0]]


def parts(container):
    for k in range(struct.unpack_from('<I', container, 28)[0]):
        at = struct.unpack_from('<I', container, 32 + 4 * k)[0]
        yield container[at:at + 4], at + 8


def classify(container):
    kind, flags = None, 0
    for fourcc, at in parts(container):
        if fourcc == b'DXIL':
            kind = struct.unpack_from('<I', container, at)[0] >> 16
        elif fourcc == b'SFI0':
            flags = struct.unpack_from('<Q', container, at)[0]
    if kind == KIND_MS:
        return 'ms2cs.py'
    if kind == KIND_CS and flags & NATIVE_LOW_PRECISION:
        return 'fix16.py'
    return None


def outputs(tool, digest):
    if tool == 'ms2cs.py':
        return [f'{digest}.emucs{ms2cs.VER}.cso', f'{digest}.emuvs{ms2cs.VER}.cso']
    return [f'{digest}.cso']


def complete(path):
    """True for a whole DXBC container; a killed run can leave a truncated file behind."""
    try:
        with open(path, 'rb') as f:
            head = f.read(28)
        return len(head) == 28 and head[:4] == b'DXBC' and struct.unpack_from('<I', head, 24)[0] == os.path.getsize(path)
    except OSError:
        return False


def converter_id():
    h = hashlib.sha256()
    for name in ('ms2cs.py', 'fix16.py'):
        h.update((TOOLS / name).read_bytes().replace(b'\r\n', b'\n'))
    return h.hexdigest()


def prune(outdir, keep):
    """Deletes converter outputs and leftover temporaries not in keep; returns how many."""
    removed = 0
    for name in os.listdir(outdir):
        if name.endswith(('.cso', '.dxil', '.ll', '.hlsl')) and name not in keep:
            os.remove(os.path.join(outdir, name))
            removed += 1
    return removed


def run(job):
    tool, outdir, paths = job
    return subprocess.run([sys.executable, '-I', str(TOOLS / tool), outdir] + paths, capture_output=True, text=True)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    game, outdir = Path(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(outdir, exist_ok=True)
    stamp = Path(outdir) / 'converter.txt'
    if not stamp.exists() or stamp.read_text().strip() != converter_id():
        print(f'converters changed, removed {prune(outdir, set())} files')
        stamp.write_text(converter_id() + '\n')
    with tempfile.TemporaryDirectory() as tmp:
        inputs = {'ms2cs.py': {}, 'fix16.py': {}}
        with open(game / PAK, 'rb') as f:
            entries = pak_entries(f)
            for archive in ARCHIVES:
                for container in shaders(f, entries[archive]):
                    tool = classify(container)
                    if tool:
                        inputs[tool].setdefault(container[4:20].hex(), container)
        keep = {name for tool, unique in inputs.items() for digest in unique for name in outputs(tool, digest)}
        print(f'removed {prune(outdir, keep)} files the game no longer uses')
        jobs = []
        for tool, unique in inputs.items():
            # Shaders a converter rejected have no outputs, so they are retried; there are few.
            todo = {digest: container for digest, container in unique.items()
                    if not all(complete(os.path.join(outdir, name)) for name in outputs(tool, digest))}
            print(f'{tool}: {len(unique)} unique shaders, {len(unique) - len(todo)} already converted')
            paths = []
            for digest, container in todo.items():
                paths.append(os.path.join(tmp, digest + '.dxil'))
                Path(paths[-1]).write_bytes(container)
            jobs += [(tool, outdir, paths[i:i + 32]) for i in range(0, len(paths), 32)]
        rejected = []
        done, total = 0, sum(len(job[2]) for job in jobs)
        with ThreadPoolExecutor(os.cpu_count()) as pool:
            for job, result in zip(jobs, pool.map(run, jobs)):
                lines = [line for line in result.stderr.splitlines() if line.startswith(tmp)]
                if result.returncode and not lines:
                    sys.exit(result.stderr)
                rejected += lines
                done += len(job[2])
                print(f'\r{done}/{total} shaders', end='', flush=True)  # the proxy's progress bar reads this
    print()
    for line in rejected:
        print('skipped', os.path.basename(line))
    count = sum(name.endswith('.cso') for name in os.listdir(outdir))
    print(f'{count} shaders in {outdir}, {len(rejected)} skipped')


if __name__ == '__main__':
    main()
