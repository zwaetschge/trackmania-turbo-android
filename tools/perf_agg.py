"""Aggregate simpleperf samples (report --sort dso,vaddr_in_file) by function, using the
symbols of the unstripped libtmturbo.so (work/syms.txt from llvm-nm -n -C).
usage: perf_agg.py <vaddr report> [top=40]
Prints the share per category and the hottest functions."""
import sys, bisect, re, collections
rep = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
addrs, names = [], []
for l in open('work/syms.txt', errors='replace'):
    p = l.rstrip('\n').split(' ', 2)
    if len(p) == 3: addrs.append(int(p[0], 16)); names.append(p[2])
fn = collections.Counter(); cat = collections.Counter(); total = 0
def category(dso, name):
    if 'libtmturbo' not in dso: return 'kernel' if 'kernel' in dso else 'adreno driver' if 'adreno' in dso else 'libc/libm' if 'bionic' in dso else 'other libs'
    if re.match(r'(sub_|extra_sub_)[0-9A-F]{8}', name): return 'lifted game code'
    if re.match(r'(am_|wp_)', name): return 'lifted DLL code'
    if name.startswith('dxvk::') or 'dxvk' in name or name.startswith('D3D11') or 'Dxbc' in name or 'Dxvk' in name: return 'DXVK'
    if name.startswith(('ctx_', 'dev_', 'sc_', 'm_', 'tm_map', 'lazy_', 'rp_split', 'gfx_', 'cap_', 'wrap', 'q_', 'view_', 'stats_', 'same_targets', 'note_targets', 'unwrap', 'sprite_', 'trace_')): return 'D3D11 bridge'
    if name.startswith(('shim_', 'tm_', 'recomp_', 'cs_', 'lookup', 'disp_', 'guest_')): return 'runtime (shims, dispatch, locks)'
    if name.startswith(('al', 'AL', 'Voice', 'Mixer')) or 'alc' in name: return 'OpenAL'
    return 'other (libtmturbo)'
for l in open(rep, errors='replace'):
    m = re.match(r'\s*([0-9.]+)%\s+(\d+)\s+(\S+)\s+(0x[0-9a-f]+)', l)
    if not m: continue
    pct, dso, a = float(m.group(1)), m.group(3), int(m.group(4), 16)
    name = '?'
    if 'libtmturbo' in dso:
        i = bisect.bisect_right(addrs, a) - 1
        name = names[i] if i >= 0 else '?'
    else: name = dso.rsplit('/', 1)[-1]
    fn[name] += pct; cat[category(dso, name)] += pct; total += pct
print(f"total {total:.1f} %")
for c, v in cat.most_common(): print(f"  {v:5.1f} %  {c}")
print("hottest functions:")
for n, v in fn.most_common(top): print(f"  {v:5.2f} %  {n[:90]}")
