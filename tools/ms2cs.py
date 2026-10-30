# Converts a mesh shader into an emulation pair for hardware without mesh shaders:
#   <outdir>/<hash>.emucs7.cso compute shader: same body, writes vertices and indices to the
#                              scratch UAV (raw, u0 space999) instead of the mesh outputs
#   <outdir>/<hash>.emuvs7.cso vertex shader: fetches vertex SV_VertexID from the vertex region
#                              (raw SRV t0 space999) and outputs the mesh shader's vertex signature
# <hash> is the original container hash, the name the proxy looks up.
#
# Each group allocates its vertices and indices with one atomic add per counter, so the output is
# compacted and the index count lands directly in DRAW_INDEXED arguments. Scratch layout (the
# proxy uses the same constants). The capacities are read from the header, which the proxy
# rewrites before every call, so the scratch can be resized without touching the shaders:
#   0    DRAW_INDEXED args {IndexCount, 1, 0, 0, 0}   only whole successful groups are drawn
#   20   vertex allocator
#   24   hypothetical index demand from vertex-failed groups (pass 0 only)
#   28   index allocator for vertex-admitted groups, separate from the bounded draw count
#   32   index capacity (a multiple of 3), set by the proxy
#   36   vertex region offset, set by the proxy
#   40   vertex region bytes, set by the proxy
#   44   vertex stride, written by the shader for the proxy's sizing
#   48   mode: bit 0 retry, bit 1 more passes, bit 2 write-list B (otherwise A)
#   52   cumulative lost groups, preserved between passes
#   56   retry-list base offset (after the geometry region)
#   60   capacity of each packed XYZ retry list
#   64   trash slot for stores past capacity
#   80/96   list A/B descriptors {count, dispatch X, Y, Z}
#   112  index region offset, set by the proxy; header ends at 116
#   index region (absolute vertex indices) and vertex region in blocks of 32 records: block v / 32
#   at (v / 32) * 32 * stride holds the first HEAD bytes of its 32 records, then the rest of each
#   (see vertex_addr). Contiguous, aligned stores cut the converted shaders' store cost (synthetic
#   benchmark). The capacity is whole blocks. Proven-half adjacent components share
#   a uint32 word
# All offsets are relative to the bound UAV address, so the regions can sit anywhere after the
# header in the scratch.
# Versioned pair names (7 = blocked vertex region, 6 = header index offset, proven-half storage,
# v4 retry header) keep incompatible pairs out. Failed groups enqueue
# their original XYZ ID for another pass, or increment lost groups on the final pass/list overflow.
# No degenerate triangles are drawn. Source UAV writes/atomics are unsupported because retries
# could replay their side effects. The proxy adapts recorded passes after completed feedback;
# the first oversized call can lose geometry. Global rasterization order remains approximate.
#
# Per-primitive outputs: indices and SV_CullPrimitive go to groupshared, and the end of the
# shader compacts the surviving triangles in order (one atomic per group, none when all are
# culled). Other per-primitive attributes (PRIMITIVEID etc.) need flat values per triangle, so
# each primitive gets a record slot after the group's vertices in the vertex region: word 0 is
# the source vertex, then the attributes. The triangle's first index is REC_FLAG | record, and
# the vertex shader reads the record for such indices (attributes are 0 on the other vertices).
#
# Supported mesh shaders: triangle topology, SetMeshOutputCounts called once, one ret, constant
# output rows. 16-bit Buffer<half> loads are converted as in fix16.py.
# Half storage uses SM5 legacy conversion intrinsics, not native 16-bit arithmetic. Only adjacent
# same-vertex writes with already-half values and exactly representable constants are selected;
# finite values retain their original precision. NaN payload identity is not guaranteed.
# usage: ms2cs.py [--keep] outdir shader.(bin|dxil)...
import os, re, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fix16

BIN = fix16.BIN
HDR_IB, HDR_LAYOUT, HDR_STRIDE = 112, 32, 44
HDR_VREQ, HDR_MODE, HDR_LOST, LIST_DESC = 24, 48, 52, 80
TRASH = 64
HEAD = 16  # bytes of each vertex record in its block's first array (a float4 position in the usual layout)
VER = 7  # pair name version: <hash>.emucs7.cso / .emuvs7.cso
KIND_CULL = 30  # SV_CullPrimitive
REC_FLAG = 0x80000000  # index bit: the vertex is a per-primitive record, see the header comment

INTERP = {0: '', 1: 'nointerpolation ', 2: 'linear ', 3: 'centroid ', 4: 'noperspective ',
          5: 'noperspective centroid ', 6: 'sample ', 7: 'noperspective sample '}
COMP = {9: ('float', 'asfloat'), 5: ('uint', ''), 4: ('int', 'asint')}


class Unsupported(Exception):
    pass


def md_nodes(ll):
    return {int(m.group(1)): m.group(2) for m in re.finditer(r'^!(\d+) = !\{(.*)\}$', ll, re.M)}


def md_ops(body):
    # Top-level comma split of a metadata node body (no nested braces in the nodes we parse).
    return [x.strip() for x in body.split(',')]


def signature(nodes, list_ref):
    """Elements of a signature list node: dicts with name, idx, comp, kind, interp, rows, cols, row, col."""
    if list_ref == 'null':
        return []
    els = []
    for ref in md_ops(nodes[int(list_ref[1:])]):
        f = md_ops(nodes[int(ref[1:])])
        idx = [int(x.split()[1]) for x in md_ops(nodes[int(f[4][1:])])]
        els.append(dict(name=f[1][2:-1], idx=idx, comp=int(f[2].split()[1]), kind=int(f[3].split()[1]),
                        interp=int(f[5].split()[1]), rows=int(f[6].split()[1]), cols=int(f[7].split()[1]),
                        row=int(f[8].split()[1]), col=int(f[9].split()[1])))
    return els


def entry_info(ll):
    nodes = md_nodes(ll)
    entry = int(re.search(r'^!dx\.entryPoints = !\{!(\d+)\}$', ll, re.M).group(1))
    e = md_ops(nodes[entry])  # fn, name, sigs, resources, tags
    sigs = md_ops(nodes[int(e[2][1:])])
    if sigs[0] != 'null':
        raise Unsupported('mesh shader has inputs')
    tags = md_ops(nodes[int(e[4][1:])])
    tagmap = {int(tags[i].split()[1]): tags[i + 1] for i in range(0, len(tags), 2)}
    ms = md_ops(nodes[int(tagmap[9][1:])])  # numthreads, maxV, maxP, topology, payload
    if int(ms[3].split()[1]) != 2:
        raise Unsupported('topology is not triangle')
    prims = signature(nodes, sigs[2])
    for el in prims:
        if el['kind'] not in (0, KIND_CULL) or (el['kind'] == 0 and el['comp'] not in COMP):
            raise Unsupported('per-primitive output %s (kind %d, component type %d)' % (el['name'], el['kind'],
                                                                                       el['comp']))
    threads = 1
    for x in md_ops(nodes[int(ms[0][1:])]):
        threads *= int(x.split()[1])
    return dict(nodes=nodes, entry=entry, e=e, tags_ref=int(e[4][1:]), tags=tags, numthreads=ms[0],
                res_ref=int(e[3][1:]) if e[3] != 'null' else None, outputs=signature(nodes, sigs[1]),
                prims=prims, threads=threads, max_prims=int(ms[2].split()[1]))


def stride_of(outputs, half=()):
    return 4 * (sum(el['rows'] * el['cols'] for el in outputs) - len(half))


def block_layout(stride):
    """(head, tail) bytes of each record in a block of 32. Strides up to HEAD are plain records."""
    head = min(stride, HEAD)
    return head, stride - head


def vertex_addr(p, index, off, stride):
    """IR computing <p>a, the UAV offset of byte off of vertex record index (see the header comment)."""
    head, tail = block_layout(stride)
    size, base = (head, off) if off < head else (tail, 32 * head + off - head)
    return ['  %sbk = lshr i32 %s, 5' % (p, index),
            '  %sbo = mul i32 %sbk, %d' % (p, p, 32 * stride),
            '  %sln = and i32 %s, 31' % (p, index),
            '  %slo = mul i32 %sln, %d' % (p, p, size),
            '  %sr = add i32 %slo, %d' % (p, p, base),
            '  %sbr = add i32 %sbo, %sr' % (p, p, p),
            '  %sa = add i32 %sbr, %%emu.vboff' % (p, p)]


def packed_offsets(outputs, half=()):
    offsets = {}
    size = 0
    for el in sorted(outputs, key=lambda el: (el['row'], el['col'])):
        for row in range(el['rows']):
            for col in range(el['cols']):
                key = el['row'] + row, el['col'] + col
                offsets[key] = size
                size += 2 if key in half or (key[0], key[1] - 1) in half else 4
    return offsets


VERTEX_STORE = re.compile(r'^  call void @dx\.op\.storeVertexOutput\.(f32|i32)\(i32 171, i32 (\d+), '
                          r'i32 ([^,]+), i8 (\d+), (?:float|i32) ([^,]+), i32 ([^)]+)\)')


def half_pairs(ll, outputs):
    """Pack adjacent stores only when both values are already exactly representable as half.

    At least one must originate in half precision; constants alone do not select an attribute.
    Phi/select propagate that proof, arithmetic does not. No semantic-name guesses or atomics.
    Positions/system values remain full-width. This runs BEFORE fix16 erases the source types.
    """
    defs = dict(re.findall(r'^  (%[\w.]+) = (.*)$', ll, re.M))
    memo = {}

    def proven(value, visiting=frozenset()):
        if not value.startswith('%'):
            try:
                number = struct.unpack('>d', bytes.fromhex(value[2:]))[0] if value.startswith('0x') else float(value)
                return struct.pack('<f', struct.unpack('<e', struct.pack('<e', number))[0]) == struct.pack('<f', number), False
            except (ValueError, OverflowError, struct.error):
                return False, False
        if value in memo:
            return memo[value]
        if value in visiting:
            return False, False
        definition = defs.get(value, '')
        if re.match(r'fpext half ', definition) or re.match(r'call float @dx\.op\.legacyF16ToF32\(i32 131,', definition):
            result = True, True
        else:
            values = []
            if definition.startswith('phi float '):
                values = re.findall(r'\[ ([^,]+), %[^ ]+ \]', definition)
            elif definition.startswith('select i1 '):
                values = re.findall(r', float ([^, ]+)', definition)
            results = [proven(v, visiting | {value}) for v in values]
            result = bool(results) and all(r[0] for r in results), any(r[1] for r in results)
        memo[value] = result
        return result

    candidates = {(el['row'] + r, el['col'] + c)
                  for el in outputs if el['kind'] == 0 and el['comp'] == 9 and 'POSITION' not in el['name'].upper()
                  for r in range(el['rows']) for c in range(0, el['cols'] - 1, 2)}
    seen, bad = set(), set()
    lines = ll.splitlines()
    for i, line in enumerate(lines):
        m = VERTEX_STORE.match(line)
        if not m:
            continue
        ty, sig, row, col, value, vertex = m.groups()
        el = outputs[int(sig)]
        if not row.isdigit():
            bad.update((el['row'] + r, el['col'] + c) for r in range(el['rows']) for c in range(0, el['cols'] - 1, 2))
            continue
        key = el['row'] + int(row), el['col'] + int(col)
        start = key if key in candidates else (key[0], key[1] - 1)
        if start not in candidates:
            continue
        other = VERTEX_STORE.match(lines[i + 1] if key == start and i + 1 < len(lines) else lines[i - 1])
        if not other:
            bad.add(start)
            continue
        oty, osig, orow, ocol, oval, overt = other.groups()
        a, b = proven(value), proven(oval)
        if (ty != 'f32' or oty != ty or (osig, orow, overt) != (sig, row, vertex) or
                int(ocol) != int(col) + (1 if key == start else -1) or not (a[0] and b[0] and (a[1] or b[1]))):
            bad.add(start)
        seen.add(start)
    return seen - bad


def atomic_add(name, offset, value):
    return ('  %s = call i32 @dx.op.atomicBinOp.i32(i32 78, %%dx.types.Handle %%emu.h, i32 0, i32 %s, i32 undef, '
            'i32 undef, i32 %s)' % (name, offset, value))


def atomic_umax(name, offset, value):
    return ('  %s = call i32 @dx.op.atomicBinOp.i32(i32 78, %%dx.types.Handle %%emu.h, i32 7, i32 %s, i32 undef, '
            'i32 undef, i32 %s)' % (name, offset, value))


def queue_failed(p):
    """Thread 0 of a group that didn't fit: queue its group ID for the next pass, or count it lost on the last pass
    or when the queue is full. The caller emits the first label; ends in block <p>done."""
    return [f'  br i1 %emu.more, label %{p}app, label %{p}lost',
            '',
            f'{p}app:',
            atomic_add(f'%{p}i', '%emu.wdesc', 1),
            f'  %{p}room = icmp ult i32 %{p}i, %emu.lcap',
            f'  br i1 %{p}room, label %{p}put, label %{p}lost',
            '',
            f'{p}put:',
            f'  %{p}e = shl i32 %{p}i, 3',
            f'  %{p}a = add i32 %emu.wlist, %{p}e',
            f'  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.h, i32 %{p}a, i32 undef, '
            f'i32 %emu.gpk, i32 %emu.g2, i32 undef, i32 undef, i8 3, i32 4)',
            # Retry dispatch dims {min(n, 65535), ceil(n / 65535), 1}; Z is preset by the proxy.
            f'  %{p}n = add i32 %{p}i, 1',
            f'  %{p}big = icmp ugt i32 %{p}n, 65535',
            f'  %{p}x = select i1 %{p}big, i32 65535, i32 %{p}n',
            f'  %{p}y0 = udiv i32 %{p}i, 65535',
            f'  %{p}y = add i32 %{p}y0, 1',
            atomic_umax(f'%{p}mx', '%emu.wdescx', f'%{p}x'),
            atomic_umax(f'%{p}my', '%emu.wdescy', f'%{p}y'),
            f'  br label %{p}done',
            '',
            f'{p}lost:',
            atomic_add(f'%{p}l', HDR_LOST, 1),
            f'  br label %{p}done',
            '',
            f'{p}done:']


def vertex_failed(ni, nxt):
    """Pass 0 records the indices a group would have needed when its vertices didn't fit (sizing feedback)."""
    return ['  br i1 %%emu.retry, label %%%s, label %%emu.vreq' % nxt,
            '',
            'emu.vreq:',
            atomic_add('%emu.vr', HDR_VREQ, ni),
            '  br label %%%s' % nxt,
            '']


def run_or_exit():
    """Group-uniform (broadcast through groupshared after a barrier): a queued group stops here."""
    return ['  %emu.okb = icmp ne i32 %emu.okv, 0',
            '  br i1 %emu.okb, label %emu.run, label %emu.skip',
            '',
            'emu.skip:',
            '  ret void',
            '',
            'emu.run:']


def alloc_prims(nv, np_, stride, records, nwords):
    """SetMeshOutputCounts with per-primitive outputs: thread 0 allocates the vertices plus, with records, one
    record slot per primitive. Indices are allocated in compact() for the primitives that survive culling. A
    group whose vertices don't fit is queued and exits."""
    cnt = nv
    L = ['  %emu.t0 = icmp eq i32 %emu.tid, 0',
         '  br i1 %emu.t0, label %emu.alloc, label %emu.join',
         '',
         'emu.alloc:']
    if records:
        L += ['  %%emu.nva = add i32 %s, %s' % (nv, np_)]
        cnt = '%emu.nva'
    L += [atomic_add('%emu.va', 20, cnt),
          '  %%emu.ve = add i32 %%emu.va, %s' % cnt,
          '  %emu.vfit = icmp ule i32 %emu.ve, %emu.vcap',
          '  br i1 %emu.vfit, label %emu.alloc.done, label %emu.vfail',
          '',
          'emu.vfail:',
          '  %%emu.npi = mul i32 %s, 3' % np_]
    L += vertex_failed('%emu.npi', 'emu.q.') + ['emu.q.:'] + queue_failed('emu.q.')
    L += ['  br label %emu.alloc.done',
          '',
          'emu.alloc.done:',
          '  %emu.ok = phi i32 [ 1, %emu.alloc ], [ 0, %emu.q.done ]',
          '  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%emu.h, i32 %d, i32 undef, '
          'i32 %d, i32 undef, i32 undef, i32 undef, i8 1, i32 4)' % (HDR_STRIDE, stride),
          '  store i32 %emu.va, i32 addrspace(3)* @emu.gsV, align 4',
          '  store i32 %emu.ok, i32 addrspace(3)* @emu.gsOk, align 4',
          '  store i32 %s, i32 addrspace(3)* @emu.gsP, align 4' % np_,
          '  %%emu.rb0 = add i32 %%emu.va, %s' % nv,
          '  store i32 %emu.rb0, i32 addrspace(3)* @emu.gsR, align 4']
    L += ['  store i32 0, i32 addrspace(3)* getelementptr inbounds ([%d x i32], [%d x i32] addrspace(3)* @emu.gsC, '
          'i32 0, i32 %d), align 4' % (nwords, nwords, w) for w in range(nwords)]
    L += ['  br label %emu.join',
          '',
          'emu.join:',
          '  call void @dx.op.barrier(i32 80, i32 9)',
          '  %emu.vbase = load i32, i32 addrspace(3)* @emu.gsV, align 4',
          '  %emu.okv = load i32, i32 addrspace(3)* @emu.gsOk, align 4',
          '  %%emu.rbase = add i32 %%emu.vbase, %s' % nv]
    L += ['  %%emu.vend = add i32 %%emu.rbase, %s' % np_] if records else ['  %emu.vend = add i32 %emu.rbase, 0']
    return L + ['  %emu.vok = icmp ule i32 %emu.vend, %emu.vcap'] + run_or_exit()


def compact(info, stride, records, nwords):
    """Replaces the single return: after a barrier, one atomic per group allocates indices for the primitives
    that weren't culled, and each one is written in its original order (prefix popcount of the cull mask).
    With records, the first index points at the primitive's record (REC_FLAG), which names the vertex. A group
    whose indices don't fit is queued and writes none."""
    T, P = info['threads'], info['max_prims']
    L = ['  call void @dx.op.barrier(i32 80, i32 9)',
         '  %emu.e.np = load i32, i32 addrspace(3)* @emu.gsP, align 4']
    for w in range(nwords):
        L += ['  %%emu.e.c%d = load i32, i32 addrspace(3)* getelementptr inbounds ([%d x i32], [%d x i32] '
              'addrspace(3)* @emu.gsC, i32 0, i32 %d), align 4' % (w, nwords, nwords, w),
              '  %%emu.e.pc%d = call i32 @dx.op.unaryBits.i32(i32 31, i32 %%emu.e.c%d)' % (w, w)]
    culled = '%emu.e.pc0'
    for w in range(1, nwords):
        L += ['  %%emu.e.tc%d = add i32 %s, %%emu.e.pc%d' % (w, culled, w)]
        culled = '%%emu.e.tc%d' % w
    L += ['  %%emu.e.kept = sub i32 %%emu.e.np, %s' % culled,
          '  %emu.e.ni = mul i32 %emu.e.kept, 3',
          '  %emu.e.empty = icmp eq i32 %emu.e.ni, 0',
          '  br i1 %emu.e.empty, label %emu.e.exit, label %emu.e.nonempty',
          '',
          'emu.e.nonempty:',
          '  %emu.e.any = icmp ne i32 %emu.e.ni, 0',
          # Group-uniform: a fully culled group skips the rest, including the second barrier.
          '  br i1 %emu.e.any, label %emu.e.live, label %emu.e.exit',
          '',
          'emu.e.live:',
          '  %emu.e.t0 = icmp eq i32 %emu.tid, 0',
          '  br i1 %emu.e.t0, label %emu.e.alloc, label %emu.e.join',
          '',
          'emu.e.alloc:',
          atomic_add('%emu.e.ia', 28, '%emu.e.ni'),
          '  %emu.e.ie = add i32 %emu.e.ia, %emu.e.ni',
          '  %emu.e.fit = icmp ule i32 %emu.e.ie, %emu.icap',
          '  br i1 %emu.e.fit, label %emu.e.drawn, label %emu.e.q.',
          '',
          'emu.e.drawn:',
          atomic_umax('%emu.e.dm', 0, '%emu.e.ie'),
          '  br label %emu.e.adone',
          '',
          'emu.e.q.:'] + queue_failed('emu.e.q.') + [
          '  br label %emu.e.adone',
          '',
          'emu.e.adone:',
          '  %emu.e.ok = phi i32 [ 1, %emu.e.drawn ], [ 0, %emu.e.q.done ]',
          '  store i32 %emu.e.ia, i32 addrspace(3)* @emu.gsI, align 4',
          '  store i32 %emu.e.ok, i32 addrspace(3)* @emu.gsOk, align 4',
          '  br label %emu.e.join',
          '',
          'emu.e.join:',
          '  call void @dx.op.barrier(i32 80, i32 9)',
          '  %emu.e.okv = load i32, i32 addrspace(3)* @emu.gsOk, align 4',
          '  %emu.e.okb = icmp ne i32 %emu.e.okv, 0',
          '  br i1 %emu.e.okb, label %emu.e.run, label %emu.e.exit',
          '',
          'emu.e.run:',
          '  %emu.e.ib = load i32, i32 addrspace(3)* @emu.gsI, align 4',
          '  %emu.e.vb = load i32, i32 addrspace(3)* @emu.gsV, align 4',
          '  %emu.e.rb = load i32, i32 addrspace(3)* @emu.gsR, align 4']
    for k in range((P + T - 1) // T):
        q = '%%emu.e.k%d.' % k
        L += ['  %sp = add i32 %%emu.tid, %d' % (q, k * T),
              '  %sv = icmp ult i32 %sp, %%emu.e.np' % (q, q),
              '  %sw = lshr i32 %sp, 5' % (q, q),
              '  %ssh = and i32 %sp, 31' % (q, q)]
        for w in range(nwords):  # culled count in lower words, and this primitive's word
            L += ['  %slt%d = icmp ugt i32 %sw, %d' % (q, w, q, w),
                  '  %sbn%d = select i1 %slt%d, i32 %%emu.e.pc%d, i32 0' % (q, w, q, w, w),
                  '  %seq%d = icmp eq i32 %sw, %d' % (q, w, q, w),
                  '  %scs%d = select i1 %seq%d, i32 %%emu.e.c%d, i32 0' % (q, w, q, w, w)]
            if w:
                L += ['  %sba%d = add i32 %s, %sbn%d' % (q, w, below, q, w),
                      '  %sca%d = or i32 %s, %scs%d' % (q, w, cw, q, w)]
            below, cw = (q + 'ba%d' % w, q + 'ca%d' % w) if w else (q + 'bn0', q + 'cs0')
        L += ['  %slm = shl i32 1, %ssh' % (q, q),
              '  %slm1 = add i32 %slm, -1' % (q, q),
              '  %sbw = and i32 %s, %slm1' % (q, cw, q),
              '  %sbc = call i32 @dx.op.unaryBits.i32(i32 31, i32 %sbw)' % (q, q),
              '  %scb = add i32 %s, %sbc' % (q, below, q),
              '  %slocal = sub i32 %sp, %scb' % (q, q, q),
              '  %sbit = and i32 %s, %slm' % (q, cw, q),
              '  %salive = icmp eq i32 %sbit, 0' % (q, q),
              '  %skeep = and i1 %sv, %salive' % (q, q, q),
              '  %sl3 = mul i32 %slocal, 3' % (q, q),
              '  %si = add i32 %%emu.e.ib, %sl3' % (q, q),
              '  br i1 %skeep, label %%emu.e.w%d, label %%emu.e.s%d' % (q, k, k),
              '',
              'emu.e.w%d:' % k,
              '  %sg = getelementptr [%d x i32], [%d x i32] addrspace(3)* @emu.gsIdx, i32 0, i32 %sp' % (q, P, P, q),
              '  %spk = load i32, i32 addrspace(3)* %sg, align 4' % (q, q),
              '  %sl0 = and i32 %spk, 255' % (q, q),
              '  %sh1 = lshr i32 %spk, 8' % (q, q),
              '  %sl1 = and i32 %sh1, 255' % (q, q),
              '  %sh2 = lshr i32 %spk, 16' % (q, q),
              '  %sl2 = and i32 %sh2, 255' % (q, q)]
        for j in range(3):
            L += ['  %sv%d = add i32 %%emu.e.vb, %sl%d' % (q, j, q, j)]
        first = q + 'v0'
        if records:
            L += ['  %sr = add i32 %%emu.e.rb, %sp' % (q, q)]
            L += vertex_addr(q + 'rec.', q + 'r', 0, stride)
            L += ['  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%emu.h, i32 %srec.a, i32 undef, '
                  'i32 %sv0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)' % (q, q),
                  '  %sx0 = or i32 %sr, %d' % (q, q, REC_FLAG - (1 << 32))]
            first = q + 'x0'
        L += ['  %so = shl i32 %si, 2' % (q, q),
              '  %sad = add i32 %so, %%emu.ibo0' % (q, q),
              '  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%emu.h, i32 %sad, i32 undef, '
              'i32 %s, i32 %sv1, i32 %sv2, i32 undef, i8 7, i32 4)' % (q, first, q, q),
              '  br label %%emu.e.s%d' % k,
              '',
              'emu.e.s%d:' % k]
    return L + ['  br label %emu.e.exit', '', 'emu.e.exit:', '  ret void']


def retry_entry():
    """Entry: the pass mode and queue layout from the header, and the group ID. A retry pass takes it from the
    read list (SV_GroupID.x + 65535 * SV_GroupID.y indexes it) and stops past the list's count."""
    L = ['  %%emu.hdr2 = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %%emu.h, '
         'i32 %d, i32 undef, i8 15, i32 4)' % HDR_MODE,
         '  %emu.mode = extractvalue %dx.types.ResRet.i32 %emu.hdr2, 0',
         '  %emu.lbase = extractvalue %dx.types.ResRet.i32 %emu.hdr2, 2',
         '  %emu.lcap = extractvalue %dx.types.ResRet.i32 %emu.hdr2, 3',
         '  %emu.m1 = and i32 %emu.mode, 1',
         '  %emu.retry = icmp ne i32 %emu.m1, 0',
         '  %emu.m2 = and i32 %emu.mode, 2',
         '  %emu.more = icmp ne i32 %emu.m2, 0',
         '  %emu.m4 = lshr i32 %emu.mode, 2',
         '  %emu.wsel = and i32 %emu.m4, 1',
         '  %emu.wd16 = shl i32 %emu.wsel, 4',
         '  %%emu.wdesc = add i32 %%emu.wd16, %d' % LIST_DESC,
         '  %%emu.wdescx = add i32 %%emu.wd16, %d' % (LIST_DESC + 4),
         '  %%emu.wdescy = add i32 %%emu.wd16, %d' % (LIST_DESC + 8),
         '  %emu.wl0 = mul i32 %emu.wsel, %emu.lcap',
         '  %emu.wl1 = shl i32 %emu.wl0, 3',
         '  %emu.wlist = add i32 %emu.lbase, %emu.wl1']
    L += ['  %%emu.n%d = call i32 @dx.op.groupId.i32(i32 94, i32 %d)' % (c, c) for c in range(3)]
    return L + ['  %emu.n1s = shl i32 %emu.n1, 16',
                '  %emu.npk = or i32 %emu.n0, %emu.n1s',
                '  br i1 %emu.retry, label %emu.rt, label %emu.nat',
                '',
                'emu.rt:',
                '  %emu.rsel = xor i32 %emu.wsel, 1',
                '  %emu.rd16 = shl i32 %emu.rsel, 4',
                '  %%emu.rdesc = add i32 %%emu.rd16, %d' % LIST_DESC,
                '  %emu.rc = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.h, '
                'i32 %emu.rdesc, i32 undef, i8 1, i32 4)',
                '  %emu.rc0 = extractvalue %dx.types.ResRet.i32 %emu.rc, 0',
                '  %emu.rcbig = icmp ugt i32 %emu.rc0, %emu.lcap',
                '  %emu.rcnt = select i1 %emu.rcbig, i32 %emu.lcap, i32 %emu.rc0',
                '  %emu.re0 = mul i32 %emu.n1, 65535',
                '  %emu.re = add i32 %emu.re0, %emu.n0',
                '  %emu.rin = icmp ult i32 %emu.re, %emu.rcnt',
                '  br i1 %emu.rin, label %emu.rload, label %emu.rexit',
                '',
                'emu.rexit:',
                '  ret void',
                '',
                'emu.rload:',
                '  %emu.rl0 = mul i32 %emu.rsel, %emu.lcap',
                '  %emu.rl1 = add i32 %emu.rl0, %emu.re',
                '  %emu.rl2 = shl i32 %emu.rl1, 3',
                '  %emu.ra = add i32 %emu.lbase, %emu.rl2',
                '  %emu.rent = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.h, '
                'i32 %emu.ra, i32 undef, i8 3, i32 4)',
                '  %emu.rpk = extractvalue %dx.types.ResRet.i32 %emu.rent, 0',
                '  %emu.rz = extractvalue %dx.types.ResRet.i32 %emu.rent, 1',
                '  %emu.rx = and i32 %emu.rpk, 65535',
                '  %emu.ry = lshr i32 %emu.rpk, 16',
                '  br label %emu.go',
                '',
                'emu.nat:',
                '  br label %emu.go',
                '',
                'emu.go:',
                '  %emu.g0 = phi i32 [ %emu.rx, %emu.rload ], [ %emu.n0, %emu.nat ]',
                '  %emu.g1 = phi i32 [ %emu.ry, %emu.rload ], [ %emu.n1, %emu.nat ]',
                '  %emu.g2 = phi i32 [ %emu.rz, %emu.rload ], [ %emu.n2, %emu.nat ]',
                '  %emu.gpk = phi i32 [ %emu.rpk, %emu.rload ], [ %emu.npk, %emu.nat ]']


def vertex_store_run(lines, start, by_id, offsets, half, head=HEAD):
    """Combine consecutive full-width stores with identical vertex/type/signature row.

    No instructions move across another operation, so every value dominates the first
    store. Packed halves keep their existing path. Widths 2/4 fit the 16-byte trash
    region at 64 without touching the retry descriptors at 80. A run stays within the
    record's head or tail, which are stored apart.
    """
    first = VERTEX_STORE.match(lines[start])
    ty, sig, row, col, value, vert = first.groups()
    el = by_id[int(sig)]
    key = el['row'] + int(row), el['col'] + int(col)
    if key in half or (key[0], key[1] - 1) in half:
        return [first]
    matches = [first]
    for line in lines[start + 1:start + 4]:
        other = VERTEX_STORE.match(line)
        if not other:
            break
        oty, osig, orow, ocol, oval, overt = other.groups()
        if (oty, osig, orow, overt) != (ty, sig, row, vert):
            break
        okey = el['row'] + int(orow), el['col'] + int(ocol)
        if (okey in half or (okey[0], okey[1] - 1) in half or
                int(ocol) != int(col) + len(matches) or offsets[okey] != offsets[key] + 4 * len(matches) or
                offsets[key] < head <= offsets[okey]):
            break
        matches.append(other)
    return matches[:4 if len(matches) == 4 else 2 if len(matches) >= 2 else 1]


def convert_cs(ll, info, vector_stores=True):
    # Retry may execute the original body again. Do not duplicate external UAV side effects.
    if re.search(r'call[^\n]*@dx\.op\.(?:rawBufferStore|bufferStore|textureStore|textureStoreSample|'
                 r'atomicBinOp|atomicCompareExchange|bufferUpdateCounter)\b', ll):
        raise Unsupported('retry cannot replay shader UAV side effects')
    outputs, half = info['outputs'], info.get('half', set())
    stride = stride_of(outputs, half)
    by_id = {i: el for i, el in enumerate(outputs)}
    offsets = packed_offsets(outputs, half)
    prims = info['prims']
    prim_ids = {i: el for i, el in enumerate(prims)}
    attrs = [el for el in prims if el['kind'] != KIND_CULL]
    rec_offsets = {k: 4 + v for k, v in packed_offsets(attrs).items()}
    if attrs and 4 + stride_of(attrs) > stride:
        raise Unsupported('per-primitive record larger than a vertex')
    nwords = (info['max_prims'] + 31) // 32

    out, label, n = [], None, 0
    skipped_stores = set()
    in_fn = False
    split_label = None
    ends = {}  # original block -> the emitted block that now holds its terminator
    rets = 0
    lines = ll.split('\n')
    for line_index, line in enumerate(lines):
        if line.startswith('define void @'):
            in_fn, label = True, '0'
            out.append(line)
            out += ['  %emu.tid = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)',
                    '  %emu.h0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, '
                    '%dx.types.ResBind { i32 0, i32 0, i32 999, i8 1 }, i32 0, i1 false)',
                    '  %emu.h = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.h0, '
                    '%dx.types.ResourceProperties { i32 4107, i32 0 })',
                    '  %%emu.hdr = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %%emu.h, '
                    'i32 %d, i32 undef, i8 7, i32 4)' % HDR_LAYOUT,
                    '  %emu.icap = extractvalue %dx.types.ResRet.i32 %emu.hdr, 0',
                    '  %emu.vboff = extractvalue %dx.types.ResRet.i32 %emu.hdr, 1',
                    '  %emu.vbbytes = extractvalue %dx.types.ResRet.i32 %emu.hdr, 2',
                    '  %%emu.vblocks = udiv i32 %%emu.vbbytes, %d' % (32 * stride),
                    '  %emu.vcap = shl i32 %emu.vblocks, 5',
                    '  %emu.icapm2 = add i32 %emu.icap, -2',
                    '  %%emu.ihdr = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %%emu.h, '
                    'i32 %d, i32 undef, i8 1, i32 4)' % HDR_IB,
                    '  %emu.ibo0 = extractvalue %dx.types.ResRet.i32 %emu.ihdr, 0',
                    '  %emu.ibo1 = add i32 %emu.ibo0, 4',
                    '  %emu.ibo2 = add i32 %emu.ibo0, 8']
            out += retry_entry()
            ends['0'] = 'emu.go'
            continue
        if in_fn and line == '}':
            in_fn = False
        m = re.match(r'^; <label>:(\d+)', line)
        if m:
            label = m.group(1)

        m = re.match(r'^  (%\d+) = call i32 @dx\.op\.groupId\.i32\(i32 94, i32 (.+?)\)', line)
        if m:
            if m.group(2) not in ('0', '1', '2'):
                raise Unsupported('dynamic SV_GroupID component')
            out.append('  %s = add i32 %%emu.g%s, 0' % m.groups())
            continue
        if '@dx.op.threadId.i32(' in line and in_fn:
            raise Unsupported('SV_DispatchThreadID')

        m = re.match(r'^  call void @dx\.op\.setMeshOutputCounts\(i32 168, i32 ([^,]+), i32 ([^)]+)\)', line)
        if m:
            if split_label is not None:
                raise Unsupported('SetMeshOutputCounts called more than once')
            split_label = label
            ends[label] = 'emu.run'
            nv, np_ = m.group(1), m.group(2)
            if prims:
                out += alloc_prims(nv, np_, stride, bool(attrs), nwords)
                continue
            # Vertices first; the indices only when they fit, so drawn index ranges stay contiguous from 0.
            out += ['  %emu.t0 = icmp eq i32 %emu.tid, 0',
                    '  br i1 %emu.t0, label %emu.alloc, label %emu.join',
                    '',
                    'emu.alloc:',
                    '  %%emu.ni = mul i32 %s, 3' % np_,
                    atomic_add('%emu.va', 20, nv),
                    '  %%emu.ve = add i32 %%emu.va, %s' % nv,
                    '  %emu.vfit = icmp ule i32 %emu.ve, %emu.vcap',
                    '  br i1 %emu.vfit, label %emu.ialloc, label %emu.vfail',
                    '',
                    'emu.ialloc:',
                    atomic_add('%emu.ia', 28, '%emu.ni'),
                    '  %emu.ie = add i32 %emu.ia, %emu.ni',
                    '  %emu.ifit = icmp ule i32 %emu.ie, %emu.icap',
                    '  br i1 %emu.ifit, label %emu.drawn, label %emu.q.',
                    '',
                    'emu.drawn:',
                    atomic_umax('%emu.dm', 0, '%emu.ie'),
                    '  br label %emu.alloc.done',
                    '',
                    'emu.vfail:']
            out += vertex_failed('%emu.ni', 'emu.q.') + ['emu.q.:'] + queue_failed('emu.q.')
            out += ['  br label %emu.alloc.done',
                    '',
                    'emu.alloc.done:',
                    '  %emu.ok = phi i32 [ 1, %emu.drawn ], [ 0, %emu.q.done ]',
                    '  %emu.iav = phi i32 [ %emu.ia, %emu.drawn ], [ 0, %emu.q.done ]',
                    '  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%emu.h, i32 %d, i32 undef, '
                    'i32 %d, i32 undef, i32 undef, i32 undef, i8 1, i32 4)' % (HDR_STRIDE, stride),
                    '  store i32 %emu.va, i32 addrspace(3)* @emu.gsV, align 4',
                    '  store i32 %emu.iav, i32 addrspace(3)* @emu.gsI, align 4',
                    '  store i32 %emu.ok, i32 addrspace(3)* @emu.gsOk, align 4',
                    '  br label %emu.join',
                    '',
                    'emu.join:',
                    '  call void @dx.op.barrier(i32 80, i32 9)',
                    '  %emu.vbase = load i32, i32 addrspace(3)* @emu.gsV, align 4',
                    '  %emu.ibase = load i32, i32 addrspace(3)* @emu.gsI, align 4',
                    '  %emu.okv = load i32, i32 addrspace(3)* @emu.gsOk, align 4',
                    '  %%emu.vend = add i32 %%emu.vbase, %s' % nv,
                    '  %emu.vok = icmp ule i32 %emu.vend, %emu.vcap'] + run_or_exit()
            ends[label] = 'emu.run'
            continue

        m = re.match(r'^  call void @dx\.op\.emitIndices\(i32 169, i32 ([^,]+), i32 ([^,]+), i32 ([^,]+), i32 ([^)]+)\)',
                     line)
        if m:
            if split_label is None:
                raise Unsupported('EmitIndices before SetMeshOutputCounts')
            n += 1
            p = '%%emu.i%d.' % n
            if prims:
                # Local indices (< 256) packed per primitive, written out by compact().
                out += ['  %ss1 = shl i32 %s, 8' % (p, m.group(3)),
                        '  %ss2 = shl i32 %s, 16' % (p, m.group(4)),
                        '  %so1 = or i32 %s, %ss1' % (p, m.group(2), p),
                        '  %spk = or i32 %so1, %ss2' % (p, p, p),
                        '  %sg = getelementptr [%d x i32], [%d x i32] addrspace(3)* @emu.gsIdx, i32 0, i32 %s'
                        % (p, info['max_prims'], info['max_prims'], m.group(1)),
                        '  store i32 %spk, i32 addrspace(3)* %sg, align 4' % (p, p)]
                continue
            out += ['  %sp = mul i32 %s, 3' % (p, m.group(1)),
                    '  %si = add i32 %%emu.ibase, %sp' % (p, p),
                    '  %sok = icmp ult i32 %si, %%emu.icapm2' % (p, p),
                    '  %sm = shl i32 %si, 2' % (p, p)]
            for k in range(3):
                out += ['  %sa%d = add i32 %sm, %%emu.ibo%d' % (p, k, p, k),
                        '  %ss%d = select i1 %sok, i32 %sa%d, i32 %d' % (p, k, p, p, k, TRASH),
                        '  %sv%d = add i32 %%emu.vbase, %s' % (p, k, m.group(2 + k)),
                        '  %sx%d = select i1 %%emu.vok, i32 %sv%d, i32 0' % (p, k, p, k),
                        '  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%emu.h, i32 %ss%d, i32 undef, '
                        'i32 %sx%d, i32 undef, i32 undef, i32 undef, i8 1, i32 4)' % (p, k, p, k)]
            continue

        m = VERTEX_STORE.match(line)
        if m:
            if line_index in skipped_stores:
                continue
            if split_label is None:
                raise Unsupported('StoreVertexOutput before SetMeshOutputCounts')
            ty, sig, row, col, val, vert = m.groups()
            if not row.isdigit():
                raise Unsupported('dynamic output row: ' + line.strip())
            el = by_id[int(sig)]
            key = el['row'] + int(row), el['col'] + int(col)
            if (key[0], key[1] - 1) in half:
                continue  # emitted by the immediately preceding component's full 32-bit store
            off = offsets[key]
            scalar = 'float' if ty == 'f32' else 'i32'
            n += 1
            p = '%%emu.v%d.' % n
            stores = vertex_store_run(lines, line_index, by_id, offsets, half, block_layout(stride)[0]) \
                if vector_stores else [m]
            skipped_stores.update(range(line_index + 1, line_index + len(stores)))
            values = [store.group(5) for store in stores]
            if key in half:
                other = VERTEX_STORE.match(lines[line_index + 1]).group(5)
                out += ['  %sh0 = call i32 @dx.op.legacyF32ToF16(i32 130, float %s)' % (p, val),
                        '  %sh1 = call i32 @dx.op.legacyF32ToF16(i32 130, float %s)' % (p, other),
                        '  %shi = shl i32 %sh1, 16' % (p, p),
                        '  %sh = or i32 %sh0, %shi' % (p, p, p)]
                ty, scalar, val = 'i32', 'i32', p + 'h'
                values = [val]
            components = ', '.join('%s %s' % (scalar, value) for value in values + ['undef'] * (4 - len(values)))
            out += ['  %si = add i32 %%emu.vbase, %s' % (p, vert),
                    '  %sok = icmp ult i32 %si, %%emu.vcap' % (p, p)]
            out += vertex_addr(p, p + 'i', off, stride)
            out += ['  %ss = select i1 %sok, i32 %sa, i32 %d' % (p, p, p, TRASH),
                    '  call void @dx.op.rawBufferStore.%s(i32 140, %%dx.types.Handle %%emu.h, i32 %ss, i32 undef, '
                    '%s, i8 %d, i32 4)' % (ty, p, components, (1 << len(values)) - 1)]
            continue

        m = re.match(r'^  call void @dx\.op\.storePrimitiveOutput\.(f32|i32)\(i32 172, i32 (\d+), i32 ([^,]+), i8 (\d+), '
                     r'(?:float|i32) ([^,]+), i32 ([^)]+)\)', line)
        if m:
            if split_label is None:
                raise Unsupported('StorePrimitiveOutput before SetMeshOutputCounts')
            ty, sig, row, col, val, prim = m.groups()
            if not row.isdigit():
                raise Unsupported('dynamic output row: ' + line.strip())
            el = prim_ids[int(sig)]
            n += 1
            p = '%%emu.p%d.' % n
            if el['kind'] == KIND_CULL:
                if ty != 'i32':
                    raise Unsupported('float SV_CullPrimitive store')
                # Set the primitive's bit in the groupshared cull mask, skipping the atomic for visible ones.
                out += ['  %sc = icmp ne i32 %s, 0' % (p, val),
                        '  br i1 %sc, label %sset, label %sdone' % (p, p, p),
                        '',
                        p[1:] + 'set:',
                        '  %ssh = and i32 %s, 31' % (p, prim),
                        '  %sm = shl i32 1, %ssh' % (p, p),
                        '  %sw = lshr i32 %s, 5' % (p, prim),
                        '  %sg = getelementptr [%d x i32], [%d x i32] addrspace(3)* @emu.gsC, i32 0, i32 %sw'
                        % (p, nwords, nwords, p),
                        '  %sold = atomicrmw or i32 addrspace(3)* %sg, i32 %sm seq_cst' % (p, p, p),
                        '  br label %sdone' % p,
                        '',
                        p[1:] + 'done:']
                ends[label] = p[1:] + 'done'
                continue
            # Other attributes go to the primitive's record, stored after the group's vertices.
            off = rec_offsets[el['row'] + int(row), el['col'] + int(col)]
            scalar = 'float' if ty == 'f32' else 'i32'
            out += ['  %sri = add i32 %%emu.rbase, %s' % (p, prim)]
            out += vertex_addr(p, p + 'ri', off, stride)
            out += ['  %ss = select i1 %%emu.vok, i32 %sa, i32 %d' % (p, p, TRASH),
                    '  call void @dx.op.rawBufferStore.%s(i32 140, %%dx.types.Handle %%emu.h, i32 %ss, i32 undef, '
                    '%s %s, %s undef, %s undef, %s undef, i8 1, i32 4)' % (ty, p, scalar, val, scalar, scalar, scalar)]
            continue

        if in_fn and line == '  ret void':
            rets += 1
            if prims:
                out += compact(info, stride, bool(attrs), nwords)
                continue

        if re.search(r'@dx\.op\.(storeVertexOutput|storePrimitiveOutput|emitIndices|setMeshOutputCounts|getMeshPayload)',
                     line):
            if line.startswith('declare '):
                continue
            if in_fn:
                raise Unsupported('unhandled mesh op: ' + line.strip())
        out.append(line)
    if split_label is None:
        raise Unsupported('no SetMeshOutputCounts')
    if prims and rets != 1:
        raise Unsupported('per-primitive outputs need a single return, found %d' % rets)

    # Split blocks' terminators now end emitted blocks: retarget phis that named the originals.
    ll = '\n'.join(out)
    ll = re.sub(r'(= phi .*)', lambda m: re.sub(r', %([\w.]+) \]', lambda b: ', %%%s ]' % ends.get(b.group(1), b.group(1)),
                                                 m.group(1)), ll)

    # Declarations, types and groupshared.
    decls = {'legacyF32ToF16': 'declare i32 @dx.op.legacyF32ToF16(i32, float) nounwind readnone',
             'threadIdInGroup.i32': 'declare i32 @dx.op.threadIdInGroup.i32(i32, i32) nounwind readnone',
             'flattenedThreadIdInGroup.i32': 'declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) nounwind readnone',
             'createHandleFromBinding': 'declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, '
                                        '%dx.types.ResBind, i32, i1) nounwind readnone',
             'annotateHandle': 'declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, '
                               '%dx.types.ResourceProperties) nounwind readnone',
             'atomicBinOp.i32': 'declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32) nounwind',
             'barrier': 'declare void @dx.op.barrier(i32, i32) noduplicate nounwind',
             'groupId.i32': 'declare i32 @dx.op.groupId.i32(i32, i32) nounwind readnone',
             'unaryBits.i32': 'declare i32 @dx.op.unaryBits.i32(i32, i32) nounwind readnone',
             'rawBufferLoad.i32': 'declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, '
                                  'i8, i32) nounwind readonly',
             'rawBufferStore.i32': 'declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, '
                                   'i32, i8, i32) nounwind',
             'rawBufferStore.f32': 'declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, '
                                   'float, float, i8, i32) nounwind'}
    if not prims:
        del decls['unaryBits.i32']
    body, tail = ll.split('\ndefine ', 1)[1].split('\n}\n', 1)
    add = [d for k, d in decls.items() if '@dx.op.%s(' % k in body and '@dx.op.%s(' % k not in tail]
    ll = ll.replace('\nattributes #', '\n' + '\n'.join(add) + '\n\nattributes #', 1)
    types = {'Handle': '{ i8* }', 'ResBind': '{ i32, i32, i32, i8 }',
             'ResourceProperties': '{ i32, i32 }', 'ResRet.i32': '{ i32, i32, i32, i32, i32 }'}
    add_types = ['%%dx.types.%s = type %s' % (name, body) for name, body in types.items()
                 if not re.search(r'^%%dx\.types\.%s = type ' % name, ll, re.M)]
    ll = ll.replace('\ndefine void @', '\n' + '\n'.join(add_types) + '\n\ndefine void @', 1)
    ll = ll.replace('\ndefine void @', '\n%struct.RWByteAddressBuffer = type { i32 }\n'
                    '@emu.gsV = external addrspace(3) global i32, align 4\n'
                    '@emu.gsI = external addrspace(3) global i32, align 4\n'
                    '@emu.gsOk = external addrspace(3) global i32, align 4\n' +
                    ('@emu.gsP = external addrspace(3) global i32, align 4\n' +
                     '@emu.gsR = external addrspace(3) global i32, align 4\n'
                     '@emu.gsC = external addrspace(3) global [%d x i32], align 4\n'
                     '@emu.gsIdx = external addrspace(3) global [%d x i32], align 4\n' % (nwords, info['max_prims'])
                     if prims else '') + '\ndefine void @', 1)

    # Metadata: cs stage, no signatures or view-ID state, NumThreads tag, the scratch UAV.
    nodes = info['nodes']
    nxt = max(int(i) for i in re.findall(r'^!(\d+) = ', ll, re.M)) + 1  # also counts distinct nodes
    ll = re.sub(r'^!dx\.viewIdState = .*\n', '', ll, flags=re.M)
    ll = ll.replace('!{!"ms", i32 6,', '!{!"cs", i32 6,')
    e = list(info['e'])
    e[2] = 'null'
    if info['res_ref'] is None:
        e[3] = '!%d' % (nxt + 2)
    ll = re.sub(r'^!%d = !\{.*\}$' % info['entry'], '!%d = !{%s}' % (info['entry'], ', '.join(e)), ll, flags=re.M)
    tags = list(info['tags'])
    for i in range(0, len(tags), 2):
        if tags[i] == 'i32 9':
            tags[i], tags[i + 1] = 'i32 4', info['numthreads']
        if tags[i] == 'i32 0':
            tags[i + 1] = 'i64 %d' % (int(tags[i + 1].split()[1]) | 0x10)  # raw/structured buffers
    if 'i32 0' not in tags[::2]:
        tags = ['i32 0', 'i64 16'] + tags
    ll = re.sub(r'^!%d = !\{.*\}$' % info['tags_ref'], '!%d = !{%s}' % (info['tags_ref'], ', '.join(tags)), ll,
                flags=re.M)
    res = md_ops(nodes[info['res_ref']]) if info['res_ref'] is not None else ['null'] * 4
    uav_ids = md_ops(nodes[int(res[1][1:])]) if res[1] != 'null' else []
    uav = ('!%d = !{i32 %d, %%struct.RWByteAddressBuffer* undef, !"", i32 999, i32 0, i32 1, i32 11, '
           'i1 false, i1 false, i1 false, null}' % (nxt, len(uav_ids)))
    new_md = [uav]
    if res[1] == 'null':
        res[1] = '!%d' % (nxt + 1)
        new_md.append('!%d = !{!%d}' % (nxt + 1, nxt))
    else:
        ll = re.sub(r'^%s = !\{.*\}$' % res[1], '%s = !{%s}' % (res[1], ', '.join(uav_ids + ['!%d' % nxt])), ll,
                    flags=re.M)
    if info['res_ref'] is None:
        new_md += ['!%d = !{%s}' % (nxt + 2, ', '.join(res)), '!dx.resources = !{!%d}' % (nxt + 2)]
    else:
        ll = re.sub(r'^!%d = !\{.*\}$' % info['res_ref'], '!%d = !{%s}' % (info['res_ref'], ', '.join(res)), ll, flags=re.M)
    return ll.rstrip('\n') + '\n' + '\n'.join(new_md) + '\n'


def vs_source(outputs, attrs=(), half=()):
    """Fetch VS. Per-primitive attributes follow the vertex fields, as in the pixel shaders' input structs; they
    are read from the record a REC_FLAG index points at (the provoking vertex) and are 0 elsewhere."""
    stride = stride_of(outputs, half)
    head, tail = block_layout(stride)
    offsets = packed_offsets(outputs, half)
    rec = {k: 4 + v for k, v in packed_offsets(attrs).items()}

    def bases(v, index):  # HLSL: <v>h/<v>t, where the record's head and tail bytes start (see vertex_addr)
        return ('uint %sk = (%s >> 5) * %d, %sl = %s & 31, %sh = %sk + %sl * %d, %st = %sk + %d + %sl * %d;'
                % (v, index, 32 * stride, v, index, v, v, v, head, v, v, 32 * head, v, tail))

    def at(v, off):
        return '%sh + %d' % (v, off) if off < head else '%st + %d' % (v, off - head)

    def fetch(v, off, cols, cast):
        if off < head < off + 4 * cols:  # the row spans head and tail
            return '%s(uint%d(%s))' % (cast, cols, ', '.join('VB.Load(%s)' % at(v, off + 4 * c) for c in range(cols)))
        return '%s(VB.%s(%s))' % (cast, 'Load%d' % cols if cols > 1 else 'Load', at(v, off))

    fields, loads, rec_loads = [], [], []
    order = sorted(outputs, key=lambda el: (el['row'], el['col']))
    for i, el in enumerate(order + sorted(attrs, key=lambda el: (el['row'], el['col']))):
        if el['comp'] not in COMP:
            raise Unsupported('output %s has component type %d' % (el['name'], el['comp']))
        if el['idx'] != list(range(el['idx'][0], el['idx'][0] + el['rows'])):
            raise Unsupported('output %s has non-consecutive semantic indices' % el['name'])
        ty, cast = COMP[el['comp']]
        vec = ty + (str(el['cols']) if el['cols'] > 1 else '')
        arr = '[%d]' % el['rows'] if el['rows'] > 1 else ''
        fields.append('  %s%s o%d%s : %s%d;' % (INTERP[el['interp']], vec, i, arr, el['name'], el['idx'][0]))
        for r in range(el['rows']):
            if i < len(order):
                off = offsets[el['row'] + r, el['col']]
                if any((el['row'] + r, el['col'] + c) in half for c in range(el['cols'])):
                    components = []
                    for c in range(el['cols']):
                        key = el['row'] + r, el['col'] + c
                        offset = offsets[key]
                        word = 'VB.Load(%s)' % at('b', offset & ~3)
                        components.append('f16tof32(%s%s)' % (word, ' >> 16' if offset & 2 else '')
                                          if key in half or (key[0], key[1] - 1) in half else '%s(%s)' % (cast, word))
                    loads.append('  o.o%d%s = %s(%s);' % (i, '[%d]' % r if arr else '', vec, ', '.join(components)))
                else:
                    loads.append('  o.o%d%s = %s;' % (i, '[%d]' % r if arr else '', fetch('b', off, el['cols'], cast)))
            else:
                off = rec[el['row'] + r, el['col']]
                rec_loads.append('    o.o%d%s = %s;' % (i, '[%d]' % r if arr else '', fetch('r', off, el['cols'], cast)))
    if not attrs:
        return ('ByteAddressBuffer VB : register(t0, space999);\n'
                'struct VOut {\n%s\n};\n'
                'VOut main(uint vid : SV_VertexID) {\n  VOut o;\n  %s\n%s\n  return o;\n}\n'
                % ('\n'.join(fields), bases('b', 'vid'), '\n'.join(loads)))
    return ('ByteAddressBuffer VB : register(t0, space999);\n'
            'struct VOut {\n%s\n};\n'
            'VOut main(uint id : SV_VertexID) {\n  VOut o = (VOut)0;\n  uint vid = id;\n'
            '  [branch] if (id >= 0x%xu) {\n    uint ri = id & 0x%xu;\n    %s\n    vid = VB.Load(%s);\n%s\n  }\n'
            '  %s\n%s\n  return o;\n}\n'
            % ('\n'.join(fields), REC_FLAG, REC_FLAG - 1, bases('r', 'ri'), at('r', 0), '\n'.join(rec_loads),
               bases('b', 'vid'), '\n'.join(loads)))


def sig_key(outputs):
    return sorted((el['name'], tuple(el['idx']), el['comp'], el['kind'], el['interp'], el['rows'], el['cols'], el['row'],
                   el['col']) for el in outputs)


def main():
    args = sys.argv[1:]
    keep = args[0] == '--keep'
    if keep: args = args[1:]
    outdir = args[0]
    os.makedirs(outdir, exist_ok=True)
    dxc = os.path.join(BIN, 'dxc.exe')
    failed = 0
    for path in args[1:]:
        data = open(path, 'rb').read()
        data = data[data.find(b'DXBC'):]
        h = data[4:20].hex()
        tmp = os.path.join(outdir, h)
        temps = [tmp + ext for ext in ('.dxil', '.ms.ll', '.cs.ll', '.vs.hlsl', '.vs.ll')]
        try:
            open(tmp + '.dxil', 'wb').write(data)
            ll = subprocess.run([dxc, '-dumpbin', tmp + '.dxil'], capture_output=True, text=True, check=True).stdout
            open(tmp + '.ms.ll', 'w').write(ll)
            if '!{!"ms", i32 6' not in ll:
                raise Unsupported('not a mesh shader')
            original = entry_info(ll)
            half = half_pairs(ll, original['outputs'])
            ll = fix16.fix(ll)
            info = entry_info(ll)  # fix16 updates resource metadata and precision flags too
            info['half'] = half
            attrs = [el for el in info['prims'] if el['kind'] != KIND_CULL]
            if attrs and 4 + stride_of(attrs) > stride_of(info['outputs'], half):
                info['half'] = set()
            open(tmp + '.cs.ll', 'w').write(convert_cs(ll, info))
            subprocess.run([os.path.join(BIN, 'dxilasm.exe'), tmp + '.cs.ll', tmp + '.emucs%d.cso' % VER], check=True)

            open(tmp + '.vs.hlsl', 'w').write(vs_source(info['outputs'], attrs, info['half']))
            subprocess.run([dxc, '-nologo', '-T', 'vs_6_6', '-Fo', tmp + '.emuvs%d.cso' % VER, '-Fc', tmp + '.vs.ll',
                            tmp + '.vs.hlsl'], check=True)
            vs = open(tmp + '.vs.ll').read()
            vn = md_nodes(vs)
            ve = md_ops(vn[int(re.search(r'^!dx\.entryPoints = !\{!(\d+)\}$', vs, re.M).group(1))])
            vs_out = signature(vn, md_ops(vn[int(ve[2][1:])])[1])
            # Attribute rows are the compiler's packing, the same packing the pixel shaders' inputs got.
            names = {(el['name'], el['idx'][0]) for el in attrs}
            vs_attrs = [el for el in vs_out if (el['name'], el['idx'][0]) in names]
            vs_out = [el for el in vs_out if (el['name'], el['idx'][0]) not in names]
            if [k[:7] for k in sig_key(vs_attrs)] != [k[:7] for k in sig_key(attrs)]:
                raise Unsupported('VS attribute outputs differ: %s vs %s' % (sig_key(attrs), sig_key(vs_attrs)))
            if sig_key(vs_out) != sig_key(info['outputs']):
                raise Unsupported('VS output signature differs:\n  MS %s\n  VS %s' % (sig_key(info['outputs']),
                                                                                  sig_key(vs_out)))
            print('%s -> %s.emucs%d.cso + .emuvs%d.cso (stride %d)' % (path, h, VER, VER,
                                                                     stride_of(info['outputs'], info['half'])))
        except (Unsupported, ValueError, subprocess.CalledProcessError) as e:
            print('%s: %s' % (path, e), file=sys.stderr)
            failed += 1
            for ext in ('.emucs%d.cso' % VER, '.emuvs%d.cso' % VER):
                if os.path.exists(tmp + ext): os.remove(tmp + ext)
        finally:
            if not keep:
                for t in temps:
                    if os.path.exists(t): os.remove(t)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
