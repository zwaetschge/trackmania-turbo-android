"""List direct E8/E9 callers of a VA and show disassembly context (read-only)."""
import sys, struct, pefile, capstone
exe = sys.argv[1]; targets = [int(x, 16) for x in sys.argv[2].split(',')]
back = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40; fwd = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x40
pe = pefile.PE(exe, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
import numpy as np
buf = np.frombuffer(img[t0:t1], dtype=np.uint8)
for tgt in targets:
    for opc in (0xE8, 0xE9):
        idx = np.nonzero(buf[:-5] == opc)[0]
        rel = buf[idx+1].astype(np.int64) | (buf[idx+2].astype(np.int64) << 8) | (buf[idx+3].astype(np.int64) << 16) | (buf[idx+4].astype(np.int64) << 24)
        rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
        dst = base + t0 + idx + 5 + rel
        for k in idx[dst == tgt]:
            site = base + t0 + int(k)
            print(f'== {"call" if opc == 0xE8 else "jmp"} {tgt:08X} from {site:08X}')
            best = None
            for s in range(site - back, site):
                ins = list(md.disasm(img[s - base:site - base + fwd], s))
                if any(i.address == site for i in ins): best = ins; break
            for i in best or []:
                print(f'  {i.address:08X}  {i.mnemonic} {i.op_str}{"   <==" if i.address == site else ""}')
