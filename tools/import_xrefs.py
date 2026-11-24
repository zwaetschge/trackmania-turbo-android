"""Find `call/jmp dword ptr [IAT]` sites for named imports and print context (read-only)."""
import sys, struct, json, pefile, capstone
exe, names = sys.argv[1], sys.argv[2].split(',')
before = int(sys.argv[3]) if len(sys.argv) > 3 else 12
after = int(sys.argv[4]) if len(sys.argv) > 4 else 12
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
slots = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        if i.name and i.name.decode() in names: slots[i.address] = i.name.decode()
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for slot, nm in slots.items():
    pat = struct.pack('<I', slot)
    for op in (b'\xff\x15', b'\xff\x25', b'\xa1', b'\x8b\x35', b'\x8b\x3d', b'\x8b\x1d', b'\x8b\x2d', b'\x8b\x0d', b'\x8b\x15'):
        needle = op + pat; off = t0
        while True:
            k = img.find(needle, off, t1)
            if k < 0: break
            off = k + 1
            print(f'== {nm} via {op.hex()} at {base+k:08X}')
            start = k - before * 4
            # resync: decode from several starts and take the one that lands on k
            for s in range(max(t0, k - 64), k):
                insns = list(md.disasm(img[s:k + after * 6], base + s))
                if any(i.address == base + k for i in insns): break
            for i in insns[-(before + after):]:
                if i.address >= base + k - before * 5 and i.address <= base + k + after * 5:
                    print(f'  {i.address:08X}  {i.mnemonic} {i.op_str}')
