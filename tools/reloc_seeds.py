"""Function-entry seeds from PE base relocations (read-only).

Every HIGHLOW relocation names a 32-bit absolute address stored in the image.
Targets inside .text whose relocation *site* lies outside .text (vtables,
callback tables, EH/SEH tables, CRT initializer lists) are code pointers that
some indirect transfer may use, so each must exist as a dispatchable entry.
Sites inside .text are reported separately (instruction immediates or jump
tables); the disassembler decides those.
"""
import sys, json, struct, collections, pefile
exe, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
secs = [(s.Name.rstrip(b'\0').decode(), base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)) for s in pe.sections]
def sec_of(va):
    for n, a, b in secs:
        if a <= va < b: return n
    return None
text = [x for x in secs if x[0] == '.text'][0]
seeds_data, seeds_text = collections.Counter(), collections.Counter()
by_site = collections.Counter()
n = 0
for blk in pe.DIRECTORY_ENTRY_BASERELOC:
    for e in blk.entries:
        if e.type != 3: continue
        n += 1
        site = base + e.rva
        val = struct.unpack_from('<I', img, e.rva)[0]
        if not (text[1] <= val < text[2]): continue
        s = sec_of(site)
        by_site[s] += 1
        (seeds_text if s == '.text' else seeds_data)[val] += 1
print('HIGHLOW relocs', n, 'code-targeting by site section', dict(by_site))
print('distinct targets from data', len(seeds_data), 'from text', len(seeds_text))
json.dump([{'address': a, 'address_hex': f'0x{a:08X}', 'note': f'reloc from data x{c}'} for a, c in sorted(seeds_data.items())], open(out, 'w'))
json.dump(sorted(seeds_text), open(out.replace('.json', '_textsites.json'), 'w'))
