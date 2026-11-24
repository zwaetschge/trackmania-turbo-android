"""Lift one of the game's bundled DLLs as an extra module.

  lift_dll.py <dll in ref/> <prefix> <base-hex>

The DLL is statically recompiled like the main executable and linked into the
same binary; the runtime maps the user's own DLL file at <base> (applying its
base relocations when that is not the preferred base) and resolves the main
image's imports of it to the lifted exports. Lifted code bakes absolute
addresses in, so every module needs its own fixed base; a copy rebased to
<base> is written to work/dll/ and that copy is what gets lifted.

Outputs: work/dll/<dll> (rebased), work/catalog_<prefix>.json, gen/<prefix>/<prefix>_recomp_*.c
"""
import json, os, subprocess, sys, pefile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, '..')
dll, prefix, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
src = os.path.join(ROOT, 'ref', dll)
os.makedirs(os.path.join(ROOT, 'work', 'dll'), exist_ok=True)
reb = os.path.join(ROOT, 'work', 'dll', dll)
pe = pefile.PE(src)
if pe.OPTIONAL_HEADER.ImageBase != base:
    pe.relocate_image(base)
    pe.OPTIONAL_HEADER.ImageBase = base
pe.write(reb)
pe = pefile.PE(reb)
assert pe.OPTIONAL_HEADER.ImageBase == base
seeds = {base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.address}
seeds.add(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint)
print(f"{dll}: base {base:08X}, {len(seeds)} export/entry seeds")
py = sys.executable
rs = os.path.join(ROOT, 'work', f'reloc_seeds_{prefix}.json')
subprocess.check_call([py, '-I', os.path.join(HERE, 'reloc_seeds.py'), reb, rs], cwd=ROOT, stdout=subprocess.DEVNULL)
for e in json.load(open(rs)): seeds.add(e['address'])
sj = os.path.join(ROOT, 'work', f'seeds_{prefix}.json')
json.dump(sorted(seeds), open(sj, 'w'))
env = dict(os.environ, PCRECOMP=os.path.join(ROOT, 'third_party', 'pcrecomp'))
cat = os.path.join(ROOT, 'work', f'catalog_{prefix}.json')
subprocess.check_call([py, '-u', os.path.join(HERE, 'run_lift.py'), 'catalog', reb, sj, cat], cwd=ROOT, env=env)
subprocess.check_call([py, '-u', os.path.join(HERE, 'run_lift.py'), 'lift', reb, cat, os.path.join(ROOT, 'gen', prefix),
                       '--prefix', prefix + '_'], cwd=ROOT, env=env, stdout=open(os.path.join(ROOT, 'work', f'lift_{prefix}.log'), 'w'))
st = json.load(open(os.path.join(ROOT, 'gen', prefix, f'{prefix}_lift_stats.json')))
print({k: st[k] for k in ('functions', 'files', 'c_lines', 'lift_failures', 'unimplemented_total')})
print('unimplemented:', dict(list(st['unimplemented'].items())[:12]))
