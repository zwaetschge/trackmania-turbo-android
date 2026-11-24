"""Write manifest.txt (relative path <TAB> size) for the dev LAN import.
usage: make_manifest.py <game_dir> <out_manifest> [subset...]"""
import os, sys
root, out = sys.argv[1], sys.argv[2]
subset = sys.argv[3:] or ['']
skip_dirs = {'InstallerResources', 'uplay_download', 'Support'}
with open(out, 'w') as f:
    for sub in subset:
        p = os.path.join(root, sub)
        if os.path.isfile(p):
            f.write(f'{sub}\t{os.path.getsize(p)}\n'); continue
        for d, dirs, files in os.walk(p):
            dirs[:] = [x for x in dirs if x not in skip_dirs]
            for fn in sorted(files):
                full = os.path.join(d, fn)
                rel = os.path.relpath(full, root)
                if rel.lower().endswith(('.exe',)) and fn != 'TrackmaniaTurbo.exe': continue
                f.write(f'{rel}\t{os.path.getsize(full)}\n')
