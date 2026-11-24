"""Pack extracted DXBC blobs into one file for the renderer self-test.
  make_shader_pak.py <dir with *.dxbc> <out.pak>      (repeated [u32 len][name][u32 size][bytes])"""
import os, struct, sys
src, out = sys.argv[1], sys.argv[2]
n = 0
with open(out, "wb") as f:
    for name in sorted(os.listdir(src)):
        if not name.endswith(".dxbc"): continue
        data = open(os.path.join(src, name), "rb").read()
        nb = name.encode()
        f.write(struct.pack("<I", len(nb))); f.write(nb); f.write(struct.pack("<I", len(data))); f.write(data); n += 1
print(n, "shaders ->", out, os.path.getsize(out) >> 20, "MB")
