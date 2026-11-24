"""Find `jmp [reg*4 + disp32]` sites whose table also has valid entries BEFORE
disp (negative-index tables, e.g. MSVC memmove's UnwindDownVec[ecx*4+28]) and
report entries that are not block starts of the containing catalog function.
usage: scan_negtables.py <catalog.json> [<catalog2.json> ...]  -> prints VAs"""
import json, sys, re, struct, bisect, pefile
pe = pefile.PE('ref/TrackmaniaTurbo.exe', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo = base + text.VirtualAddress; hi = lo + text.Misc_VirtualSize
funcs = []
for p in sys.argv[1:]:
    for f in json.load(open(p))['functions']:
        funcs.append((f['address'], f['end'], {b[0] for b in f['blocks']}, p))
funcs.sort(key=lambda t: t[0]); starts = [f[0] for f in funcs]
def owner(va):
    i = bisect.bisect_right(starts, va) - 1
    while i >= 0 and i > bisect.bisect_right(starts, va) - 40:
        if funcs[i][0] <= va < funcs[i][1]: return funcs[i]
        i -= 1
    return None
code = img[lo - base:hi - base]
hits = {}
for m in re.finditer(rb'\xff\x24[\x85\x8d\x95\x9d\xad\xb5\xbd]', code, re.S):
    site = lo + m.start(); disp = struct.unpack_from('<I', code, m.start() + 3)[0]
    if not (lo <= disp < hi): continue
    f = owner(site)
    if not f: continue
    back = []
    for k in range(1, 33):
        a = disp - 4 * k
        t = struct.unpack_from('<I', img, a - base)[0]
        # a real negative-index entry points into the owning function itself;
        # anything else is the neighbouring function's table laid out before ours
        if f[0] <= t < f[1] + 0x200: back.append(t)
        else: break
    if not back: continue
    missing = [t for t in back if t not in f[2]]
    if missing: hits.setdefault((f[0], f[3]), []).append((site, disp, len(back), missing))
for (fa, p), lst in sorted(hits.items()):
    for site, disp, nb, missing in lst:
        print(f"func {fa:08X} [{p.split('/')[-1]}] jmp@{site:08X} table {disp:08X} back_entries={nb} missing={[hex(x) for x in missing[:8]]}")
print(f"functions affected: {len(hits)}")
json.dump(sorted({fa for (fa, p) in hits}), open('work/negtable_funcs.json', 'w'))
