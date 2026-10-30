"""Find direct E8/E9 call/jmp sites to a VA and disassemble around them.
usage: callers.py exe target_va_hex [before] [after]"""
import sys, re, pefile, capstone
exe, tgt = sys.argv[1], int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x100
pe = pefile.PE(exe, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text')); data = text.get_data(); tva = text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
hits = [m.start() for m in re.finditer(rb'[\xe8\xe9]', data)
        if base + tva + m.start() + 5 + int.from_bytes(data[m.start()+1:m.start()+5], 'little', signed=True) == tgt]
print("sites:", [hex(base + tva + h) for h in hits])
for h in hits:
    print("=" * 30, hex(base + tva + h))
    for ins in md.disasm(data[max(0, h - before):h + after], base + tva + max(0, h - before)):
        print("%x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
