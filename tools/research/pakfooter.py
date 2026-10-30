"""Print UE4 .pak footer info. usage: pakfooter.py file.pak"""
import sys, struct
f = open(sys.argv[1], 'rb'); f.seek(0, 2); size = f.tell()
f.seek(size - 512); tail = f.read()
i = tail.rfind(struct.pack('<I', 0x5A6F12E1))
assert i >= 17, "magic not found"
guid = tail[i-17:i-1].hex(); enc = tail[i-1]
ver, idxoff, idxsize = struct.unpack_from('<IQQ', tail, i + 4)
print(f"size={size} version={ver} encryptedIndex={enc} keyGuid={guid} indexOffset={idxoff} indexSize={idxsize}")
comp = tail[i + 4 + 4 + 16 + 20:]
print("compression:", [comp[j:j+32].rstrip(b'\0').decode(errors='replace') for j in range(0, len(comp), 32) if comp[j:j+32].strip(b'\0')])
