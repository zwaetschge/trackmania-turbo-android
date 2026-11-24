"""Classify overlapping basic blocks in a catalog: 'aligned' = next block starts on
an instruction boundary of the previous block (an UNSPLIT block: the lifter would
re-execute code on fallthrough), 'misaligned' = overlapping decodes (garbage seed).
usage: chk_overlap.py <catalog.json> [--list N]"""
import json, sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE('ref/TrackmaniaTurbo.exe', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)
cat = json.load(open(sys.argv[1]))
nlist = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 8
al_f = set(); mis_f = set(); al = mis = 0
for f in cat['functions']:
    bl = sorted(f['blocks'], key=lambda b: b[0])
    for i in range(len(bl) - 1):
        s, e = bl[i][0], bl[i][1]
        # any later block starting inside [s,e)
        for j in range(i + 1, len(bl)):
            ns = bl[j][0]
            if ns >= e: break
            bounds = set(); a = s
            for (addr, size, _m, _o) in md.disasm_lite(img[s - base:e - base], s):
                bounds.add(addr)
            if ns in bounds: al += 1; al_f.add(f['address'])
            else: mis += 1; mis_f.add(f['address'])
print(f"{sys.argv[1]}: functions={len(cat['functions'])} aligned_overlaps={al} in {len(al_f)} funcs; misaligned_overlaps={mis} in {len(mis_f)} funcs")
print(" aligned examples:", [hex(x) for x in sorted(al_f)[:nlist]])
print(" misaligned examples:", [hex(x) for x in sorted(mis_f)[:nlist]])
json.dump({'aligned': sorted(al_f), 'misaligned': sorted(mis_f)}, open(sys.argv[1] + '.overlap.json', 'w'))
