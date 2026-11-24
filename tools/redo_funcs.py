"""Re-disassemble selected functions with pcrecomp's disassembler and replace
their entries IN PLACE in a catalog (function count and order unchanged, so the
lift re-slices nothing: only the files holding those functions are rewritten).
usage: redo_funcs.py <catalog.json> <vas.json | hexva...> [--check N]
  --check N : do not write; re-disassemble N evenly spaced functions and report
              how many differ from the catalog (sanity check of the method)."""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.path.join(HERE, '..', 'third_party', 'pcrecomp')
for sub in ('pe', 'disasm'): sys.path.insert(0, os.path.join(PCRECOMP, 'tools', sub))
from pe_analyze import analyze_pe, build_iat_map
from disasm32 import Disassembler
exe = os.path.join(HERE, '..', 'ref', 'TrackmaniaTurbo.exe')
info = analyze_pe(exe); iat = build_iat_map(info); data = open(exe, 'rb').read()
dis = Disassembler(data, info.image_base, info.sections)
args = sys.argv[1:]
check = int(args[args.index('--check') + 1]) if '--check' in args else 0
if check: args = args[:args.index('--check')]
cat_path = args[0]
cat = json.load(open(cat_path)); fns = cat['functions']
def entry(va, old):
    f = dis.disassemble_function(va, iat)
    if not f or not f.blocks: return None
    # find_functions clips 'end'/'size' when it carves aliases; the blocks are
    # what the lifter decodes, so keep the catalog's own end/size.
    return {'address': f.address, 'end': old.get('end', f.end), 'size': old.get('size', f.size),
            'entry_kind': old.get('entry_kind', 'start'), 'is_thunk': f.is_thunk,
            'jump_targets': sorted(f.jump_targets), 'calls_to': sorted(f.calls_to),
            'blocks': [[b.start, b.end, sorted(set(b.successors)), int(b.is_exit)]
                       for b in sorted(f.blocks.values(), key=lambda b: b.start)]}
if check:
    step = max(1, len(fns) // check); diff = 0; n = 0; ex = []
    for old in fns[::step]:
        new = entry(old['address'], old); n += 1
        if new is None or any(new[k] != old[k] for k in ('blocks', 'jump_targets', 'calls_to', 'is_thunk')):
            diff += 1
            if len(ex) < 8: ex.append(hex(old['address']))
    print(f"checked {n}: differ {diff} examples {ex}")
    sys.exit(0)
vas = json.load(open(args[1])) if args[1].endswith('.json') else [int(x, 16) for x in args[1:]]
want = set(vas); done = 0; missing = set(want)
for i, old in enumerate(fns):
    if old['address'] in want:
        missing.discard(old['address'])
        new = entry(old['address'], old)
        if new is None: print(f"  {old['address']:08X}: disassembly failed, kept"); continue
        if new != old:
            print(f"  {old['address']:08X}: blocks {len(old['blocks'])} -> {len(new['blocks'])}, end {old['end']:08X} -> {new['end']:08X}")
            fns[i] = new; done += 1
json.dump(cat, open(cat_path, 'w'))
print(f"replaced {done} of {len(want)} (not in catalog: {[hex(x) for x in sorted(missing)][:10]})")
