"""Find references to a 32-bit little-endian constant (e.g. a function VA) in the
ref exe, mapping file offsets to VAs. usage: find_ref.py <hexval> [<hexval>...]"""
import os, sys, struct
HERE=os.path.dirname(os.path.abspath(__file__))
PCRECOMP=os.path.join(HERE,'..','third_party','pcrecomp')
for sub in ('pe','disasm'): sys.path.insert(0, os.path.join(PCRECOMP,'tools',sub))
from pe_analyze import analyze_pe
exe=os.path.join(HERE,'..','ref','TrackmaniaTurbo.exe')
info=analyze_pe(exe); data=open(exe,'rb').read()
# build offset->VA map from sections
secs=[]
for s in info.sections:
    # support attr names across pe_analyze versions
    va=getattr(s,'virtual_address',None) or getattr(s,'va',None) or s.VirtualAddress
    raw=getattr(s,'pointer_to_raw_data',None) or getattr(s,'raw',None) or getattr(s,'PointerToRawData',None)
    rsz=getattr(s,'size_of_raw_data',None) or getattr(s,'raw_size',None) or getattr(s,'SizeOfRawData',None)
    nm=getattr(s,'name','?')
    secs.append((raw,rsz,va,nm))
def off2va(off):
    for raw,rsz,va,nm in secs:
        if raw is not None and raw<=off<raw+rsz:
            return info.image_base+va+(off-raw), nm
    return None,None
for arg in sys.argv[1:]:
    val=int(arg,16)
    needle=struct.pack('<I', val)
    print(f"\n=== refs to 0x{val:08X} (bytes {needle.hex()}) ===")
    i=0; n=0
    while True:
        j=data.find(needle, i)
        if j<0: break
        va,nm=off2va(j)
        print(f"  file 0x{j:06X}  VA {('0x%08X'%va) if va else '??':>10}  sect {nm}")
        i=j+1; n+=1
        if n>40: print("  ...more"); break
    if n==0: print("  (none)")
