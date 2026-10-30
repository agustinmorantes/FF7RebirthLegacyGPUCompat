"""Disassemble a VA range. usage: dis.py exe start_va_hex length [grep_regex]"""
import sys, re, pefile, capstone
exe, va, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 0)
pat = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
pe = pefile.PE(exe, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
off = pe.get_offset_from_rva(va - base); data = open(exe, 'rb').read()[off:off + n]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for ins in md.disasm(data, va):
    line = "%x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str)
    if not pat or pat.search(line): print(line)
