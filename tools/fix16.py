# Converts shaders whose only 16-bit use is typed-buffer loads of half (Buffer<half>) to load
# float instead. Typed loads convert from the view format in hardware, so the result is
# identical and the shader no longer needs native 16-bit ops. Refuses anything else that uses
# half or i16. Writes <outdir>/<original container hash>.cso, the name the proxy looks up.
# usage: fix16.py outdir shader.(bin|dxil)...   (DXIL container, possibly after a header)
import os, re, subprocess, sys

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dxc-1.9.2609', 'bin', 'x64')
HALF_BUF = re.compile(r'^%"class\.Buffer<(?:half|vector<half, \d> )>" = type ')


def fix(src):
    has_f32_type = '%dx.types.ResRet.f32 = type' in src
    has_f32_load = 'declare %dx.types.ResRet.f32 @dx.op.bufferLoad.f32' in src
    lines = []
    for line in src.split('\n'):
        if line.startswith('%dx.types.ResRet.f16 = type'):
            if has_f32_type: continue
            line = '%dx.types.ResRet.f32 = type { float, float, float, float, i32 }'
        if line.startswith('declare %dx.types.ResRet.f16 @dx.op.bufferLoad.f16') and has_f32_load:
            continue
        # %a = fpext half %b to float  ->  identity (x + -0.0 == x for every x)
        line = re.sub(r'= fpext half (%\d+) to float', r'= fadd float \1, -0.000000e+00', line)
        # Buffer<half> / Buffer<vector<half, N> > element type -> float
        if HALF_BUF.match(line):
            name, body = line.split(' = type ', 1)
            line = name + ' = type ' + body.replace('half', 'float')
        line = line.replace('ResRet.f16', 'ResRet.f32').replace('bufferLoad.f16', 'bufferLoad.f32')
        # AnnotateHandle props for TypedBuffer: component type F16 (8) -> F32 (9), any component count
        line = re.sub(r'ResourceProperties \{ i32 10, i32 (\d+) \}',
                      lambda m: 'ResourceProperties { i32 10, i32 %d }' % (int(m.group(1)) + ((int(m.group(1)) & 0xff) == 8)),
                      line)
        lines.append(line)
    out = '\n'.join(lines)

    # Resource metadata element type of the half buffers: !{i32 0, i32 8} -> !{i32 0, i32 9}
    for m in re.finditer(r'^!\d+ = !\{i32 \d+, (?:\[\d+ x )?%"class\.Buffer<(?:half|vector<half, \d> )>"\]?\* undef, .*, (!\d+)\}$',
                         out, re.M):
        out = re.sub(r'^(%s = !\{i32 0, i32) 8\}$' % re.escape(m.group(1)), r'\1 9}', out, flags=re.M)

    # Shader flags (entry-point tag 0): drop LowPrecisionPresent (0x20) and UseNativeLowPrecision (0x800000).
    out = re.sub(r'(^!\d+ = !\{i32 0, )i64 (\d+)', lambda m: '%si64 %d' % (m.group(1), int(m.group(2)) & ~0x800020),
                 out, count=1, flags=re.M)

    body = '\n'.join(re.sub(r'"[^"]*"', '', l) for l in out.split('\n') if not l.startswith((';', 'target')))
    left = re.findall(r'^.*\b(?:half|i16)\b.*$', body, re.M)
    if left:
        raise ValueError('still uses 16-bit types:\n' + '\n'.join(left[:10]))
    return out


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    failed = 0
    for path in sys.argv[2:]:
        data = open(path, 'rb').read()
        data = data[data.find(b'DXBC'):]
        h = data[4:20].hex()
        tmp = os.path.join(outdir, h)
        open(tmp + '.dxil', 'wb').write(data)
        ll = subprocess.run([os.path.join(BIN, 'dxc.exe'), '-dumpbin', tmp + '.dxil'], capture_output=True, text=True,
                            check=True).stdout
        try:
            open(tmp + '.ll', 'w').write(fix(ll))
            subprocess.run([os.path.join(BIN, 'dxilasm.exe'), tmp + '.ll', tmp + '.cso'], check=True)
            print('%s -> %s.cso' % (path, h))
        except (ValueError, subprocess.CalledProcessError) as e:
            print('%s: %s' % (path, e), file=sys.stderr)
            failed += 1
        finally:
            for ext in ('.dxil', '.ll'):
                if os.path.exists(tmp + ext): os.remove(tmp + ext)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
