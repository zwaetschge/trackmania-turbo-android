"""Approximate linear-sweep mnemonic census of a PE32 .text section (read-only)."""
import sys, json, collections, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
text = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]
data = text.get_data()[:text.Misc_VirtualSize]
base = pe.OPTIONAL_HEADER.ImageBase + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
cnt = collections.Counter(); groups = collections.Counter(); total = 0
md.detail = True
for insn in md.disasm(data, base):
    if insn.id == 0: continue
    total += 1
    cnt[insn.mnemonic] += 1
    for g in insn.groups:
        groups[insn.group_name(g)] += 1
json.dump({'total': total, 'mnemonics': cnt.most_common(), 'groups': groups.most_common()}, open(sys.argv[2], 'w'), indent=0)
print('total', total, 'distinct', len(cnt))
print('groups', groups.most_common(40))
