"""Add missed functions (indirect-only call targets the disassembler skipped)
to the catalog, so re-lift picks them up. usage: add_funcs.py catalog.json VA [VA...]
Each VA is disassembled into basic blocks until a terminator."""
import json, sys, bisect
import pefile, capstone
cat_path = sys.argv[1]
import os
if len(sys.argv) == 3 and sys.argv[2].endswith('.json'):
    vas = json.load(open(sys.argv[2]))
else:
    vas = [int(x, 16) for x in sys.argv[2:]]
pe = pefile.PE('ref/TrackmaniaTurbo.exe', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
cat = json.load(open(cat_path))
have = set(f['address'] for f in cat['functions'])
TERM = {'ret', 'retn', 'retf', 'jmp', 'int3'}
def trace_blocks(start):
    """Linear decode from start, following intra-function jcc/jmp, collecting blocks."""
    blocks = {}; worklist = [start]; seen = set()
    lo, hi = start, start
    while worklist:
        a = worklist.pop()
        if a in seen: continue
        seen.add(a)
        succ = []; is_exit = 0; cur = a
        while True:
            ins = next(md.disasm(img[cur-base:cur-base+16], cur), None)
            if ins is None: is_exit = 1; break
            hi = max(hi, cur + ins.size)
            m = ins.mnemonic
            if m in ('ret','retn','retf'): is_exit = 1; cur += ins.size; break
            if m == 'int3': is_exit = 1; break
            if m == 'jmp':
                op = ins.op_str
                if op.startswith('0x'):
                    t = int(op,16)
                    if start <= t < start+0x2000: succ=[t]; worklist.append(t)
                else: is_exit = 1
                cur += ins.size; break
            if m.startswith('j') or m.startswith('loop'):
                op = ins.op_str
                if op.startswith('0x'):
                    t = int(op,16); nxt = cur+ins.size
                    succ=[t,nxt]; worklist += [t,nxt]
                cur += ins.size; break
            cur += ins.size
        blocks[a] = [a, cur, sorted(set(succ)), is_exit]
    return blocks, start, hi
added = 0
starts_sorted = sorted(have)
for va in vas:
    if va in have: continue
    blocks, lo, hi = trace_blocks(va)
    i = bisect.bisect_right(starts_sorted, va)
    nxt = starts_sorted[i] if i < len(starts_sorted) else cat['code_end']
    end = min(hi, nxt)
    cat['functions'].append({'address': va, 'end': end, 'size': end-va, 'entry_kind':'start',
        'is_thunk': False, 'jump_targets': [], 'calls_to': [],
        'blocks': [[b[0], min(b[1], end), b[2], b[3]] for b in sorted(blocks.values())]})
    have.add(va); added += 1
cat['functions'].sort(key=lambda f: f['address'])
json.dump(cat, open(cat_path, 'w'))
print(f'added {added} functions; total {len(cat["functions"])}')
