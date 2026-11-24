import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
va = int(sys.argv[2],16); n = int(sys.argv[3]) if len(sys.argv)>3 else 30
if va < base: va += base
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i,ins in enumerate(md.disasm(img[va-base:va-base+n*16], va)):
    if i>=n: break
    print(f'{ins.address:08X}  {ins.mnemonic} {ins.op_str}')
