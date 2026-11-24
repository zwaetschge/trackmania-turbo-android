"""Build/extend an extra catalog using pcrecomp's REAL disassembler
(disasm32.disassemble_function) for correct function boundaries, instead of a
hand-rolled tracer. usage: add_funcs2.py <extra_catalog.json> <vas.json>"""
import json, os, sys
HERE=os.path.dirname(os.path.abspath(__file__))
PCRECOMP=os.path.join(HERE,'..','third_party','pcrecomp')
for sub in ('pe','disasm'): sys.path.insert(0, os.path.join(PCRECOMP,'tools',sub))
from pe_analyze import analyze_pe, build_iat_map
import disasm32
from disasm32 import Disassembler
exe='ref/TrackmaniaTurbo.exe'
info=analyze_pe(exe); iat=build_iat_map(info); data=open(exe,'rb').read()
dis=Disassembler(data, info.image_base, info.sections)
cat_path, vas_path = sys.argv[1], sys.argv[2]
vas=json.load(open(vas_path))
cat=json.load(open(cat_path)) if os.path.exists(cat_path) and os.path.getsize(cat_path)>2 else {
    'image_base':info.image_base,'code_start':info.code_start,'code_end':info.code_end,'functions':[]}
have=set(f['address'] for f in cat['functions'])
added=0; failed=0
for va in vas:
    if va in have: continue
    f=dis.disassemble_function(va, iat)
    if not f or not f.blocks: failed+=1; continue
    cat['functions'].append({'address':f.address,'end':f.end,'size':f.size,
        'entry_kind':'start','is_thunk':f.is_thunk,
        'jump_targets':sorted(f.jump_targets),'calls_to':sorted(f.calls_to),
        'blocks':[[b.start,b.end,sorted(set(b.successors)),int(b.is_exit)] for b in sorted(f.blocks.values(), key=lambda b:b.start)]})
    have.add(va); added+=1
cat['functions'].sort(key=lambda f:f['address'])
json.dump(cat, open(cat_path,'w'))
print(f'added {added}, failed {failed}, total {len(cat["functions"])}')
