"""Dump dwords from the ref exe at a VA. usage: rd.py <hexva> [count]"""
import sys, struct, pefile
pe = pefile.PE('ref/TrackmaniaTurbo.exe', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
va = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 16
for i in range(n):
    a = va + 4 * i
    print(f"  {a:08X}: {struct.unpack_from('<I', img, a - base)[0]:08X}")
