#!/usr/bin/env python3
"""Linear x86 disassembly of an address range of the game exe (capstone).
usage: dis_range.py <hex start va> <hex end va>"""
import sys
import capstone
import pefile
pe = pefile.PE('ref/TrackmaniaTurbo.exe', fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
a, b = int(sys.argv[1], 16), int(sys.argv[2], 16)
for s in pe.sections:
    va = base + s.VirtualAddress
    if va <= a < va + s.SizeOfRawData:
        data = s.get_data()[a - va:b - va]
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for i in md.disasm(data, a):
            print('%08X  %-8s %s' % (i.address, i.mnemonic, i.op_str))
