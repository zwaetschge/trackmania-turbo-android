"""Disassemble given VAs from the ref exe using pcrecomp's disassembler.
usage: dis_dump.py <hexva> [<hexva> ...]"""
import os, sys
HERE=os.path.dirname(os.path.abspath(__file__))
PCRECOMP=os.path.join(HERE,'..','third_party','pcrecomp')
for sub in ('pe','disasm'): sys.path.insert(0, os.path.join(PCRECOMP,'tools',sub))
from pe_analyze import analyze_pe, build_iat_map
from disasm32 import Disassembler
exe=os.path.join(HERE,'..','ref','TrackmaniaTurbo.exe')
info=analyze_pe(exe); iat=build_iat_map(info); data=open(exe,'rb').read()
dis=Disassembler(data, info.image_base, info.sections)
for arg in sys.argv[1:]:
    va=int(arg,16)
    f=dis.disassemble_function(va, iat)
    print(f"\n===== sub_{va:08X}  size={f.size if f else '?'} blocks={len(f.blocks) if f else 0} =====")
    if not f: print("  (disasm failed)"); continue
    insns=[]
    for b in sorted(f.blocks.values(), key=lambda b:b.start):
        for ins in b.instructions: insns.append(ins)
    insns.sort(key=lambda i:i.address)
    for ins in insns:
        print(f"  0x{ins.address:08X}: {ins.mnemonic:7s} {ins.op_str}")
