"""Extract whole text blocks (script/XML sources) that contain a keyword from a guest memory dump.
usage: memtext.py <dump dir> <keyword> <out dir>   -> out/<va>.txt per distinct block, prints a summary"""
import sys, os, mmap, re, hashlib
d, key, out = sys.argv[1], sys.argv[2].encode(), sys.argv[3]
os.makedirs(out, exist_ok=True)
import glob
IDX = (glob.glob(os.path.join(d, 'memdump*.idx')) or [d])[0] if os.path.isdir(d) else d   # a dump dir or one .idx file
runs = [tuple(int(x, 16) for x in l.split()) for l in open(IDX)]
f = open(IDX[:-4] + '.bin', 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
def va_of(off):
    for va, ln, o in runs:
        if o <= off < o + ln: return va + off - o
TEXT = set(range(0x20, 0x7f)) | {9, 10, 13} | set(range(0x80, 0x100))
def block(i):
    a = i
    while a > 0 and m[a - 1] in TEXT: a -= 1
    b = i
    n = len(m)
    while b < n and m[b] in TEXT: b += 1
    return a, b
seen, pos, done_until = {}, 0, -1
while True:
    i = m.find(key, pos)
    if i < 0: break
    if i < done_until: pos = i + 1; continue
    a, b = block(i); done_until = b; pos = b
    if b - a < 200: continue
    t = m[a:b]; h = hashlib.sha1(t).hexdigest()
    if h in seen: seen[h][1] += 1; continue
    va = va_of(a); seen[h] = [va, 1, b - a]
    open(os.path.join(out, f"{va:08X}.txt"), 'wb').write(t)
for h, (va, n, ln) in sorted(seen.items(), key=lambda x: x[1][0]):
    t = open(os.path.join(out, f"{va:08X}.txt"), 'rb').read(160).decode('latin1').replace('\n', ' ').replace('\r', '')
    print(f"{va:08X} len {ln:7d} x{n}  {t[:120]}")
