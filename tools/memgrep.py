"""Search a guest memory dump (F11 -> memdump.bin / memdump.idx).
usage: memgrep.py <dump dir> <text> [context_bytes]     ASCII and UTF-16LE hits with guest VAs
       memgrep.py <dump dir> @<hex va> <len>            print the bytes at a guest VA as text
       memgrep.py <dump dir> x<hex bytes>               search raw bytes"""
import sys, os, mmap, re
d = sys.argv[1]
import glob
IDX = (glob.glob(os.path.join(d, 'memdump*.idx')) or [d])[0] if os.path.isdir(d) else d   # a dump dir or one .idx file
runs = [tuple(int(x, 16) for x in l.split()) for l in open(IDX)]
f = open(IDX[:-4] + '.bin', 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def va_of(off):
    lo, hi = 0, len(runs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        va, ln, o = runs[mid]
        if off < o: hi = mid - 1
        elif off >= o + ln: lo = mid + 1
        else: return va + off - o
    return None


def off_of(va):
    for v, ln, o in runs:
        if v <= va < v + ln: return o + va - v, v + ln - va
    return None, 0


def show(b):
    return re.sub(rb'[^\x09\x0a\x20-\x7e]+', b'\xc2\xb7', b).decode('utf-8', 'replace')


arg = sys.argv[2]
if arg.startswith('@'):
    va = int(arg[1:], 16); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 512
    o, avail = off_of(va)
    if o is None: sys.exit('not in dump')
    sys.stdout.write(show(m[o:o + min(n, avail)]) + '\n')
else:
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 96
    pats = [bytes.fromhex(arg[1:])] if arg.startswith('x') and re.fullmatch(r'x[0-9a-fA-F]+', arg) else [arg.encode(), arg.encode('utf-16le')]
    for kind, pat in enumerate(pats):
        pos = 0; n = 0
        while True:
            i = m.find(pat, pos)
            if i < 0: break
            n += 1
            if n <= 200:
                a, b = max(0, i - ctx), min(len(m), i + len(pat) + ctx)
                print(f"{va_of(i):08X} {'w' if kind else 'a'}: {show(m[a:b])}")
            pos = i + 1
        print(f"-- {n} hit(s) for {'utf16' if kind else 'bytes'} pattern")
