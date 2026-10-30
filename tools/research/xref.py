"""Find RIP-relative references to a file offset (string) in a PE and disassemble around them.
usage: xref.py exe file_offset_hex [before_bytes] [after_bytes]"""
import sys, re, pefile, capstone
exe, off = sys.argv[1], int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x180
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x80
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
target = pe.get_rva_from_offset(off)
print("sections:", [(s.Name.rstrip(b'\0').decode(), hex(s.VirtualAddress), hex(s.Misc_VirtualSize)) for s in pe.sections])
print("target rva", hex(target))
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
data = text.get_data(); tva = text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
hits = []
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', data):
    i = m.start(); disp = int.from_bytes(data[i+3:i+7], 'little', signed=True)
    if tva + i + 7 + disp == target: hits.append(i)
print("xrefs:", [hex(base + tva + h) for h in hits])
for h in hits:
    start = max(0, h - before)
    print("=" * 30, hex(base + tva + h))
    # resync: disassemble from start, print only from a few instr before the window
    for ins in md.disasm(data[start:h + after], base + tva + start):
        print("%x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
