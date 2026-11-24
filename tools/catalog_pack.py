#!/usr/bin/env python3
"""Function maps of the lift (addresses only: function starts, basic-block ranges, branch
and call targets -- no code, no bytes of the game).
  catalog_pack.py pack   <catalog.json> <out.json.xz>    (developer: after the catalog changed)
  catalog_pack.py unpack <in.json.xz>   <catalog.json>   (build: before lifting)
The lifter re-decodes every function from the user's own TrackmaniaTurbo.exe."""
import json, lzma, sys
cmd, src, dst = sys.argv[1:4]
if cmd == "pack":
    d = json.load(open(src))
    with lzma.open(dst, "wt", preset=9 | lzma.PRESET_EXTREME) as f:
        json.dump(d, f, separators=(",", ":"))
elif cmd == "unpack":
    with lzma.open(src, "rt") as f:
        d = json.load(f)
    json.dump(d, open(dst, "w"))
else:
    raise SystemExit(__doc__)
print(cmd, dst, len(d["functions"]), "functions")
