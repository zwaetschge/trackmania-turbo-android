#!/usr/bin/env python3
"""Script/markup lines in a guest memory dump that match a regular expression.

    tools/memlines.py <dump dir> '<regex>' [max_lines=80] [-n]   (-n: also print the guest address)

Scans memdump*.bin for the pattern, takes the whole text line around every hit and prints
the distinct lines that look like ManiaScript or manialink XML, with their counts.
"""
import collections
import glob
import mmap
import os
import re
import sys

d, pat = sys.argv[1], sys.argv[2].encode()
args = [a for a in sys.argv[3:] if a != '-n']
limit = int(args[0]) if args else 80
show_va = '-n' in sys.argv
idx = glob.glob(os.path.join(d, 'memdump*.idx'))[0]
runs = [tuple(int(x, 16) for x in l.split()) for l in open(idx)]
f = open(idx[:-4] + '.bin', 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def va_of(off):
    for va, ln, o in runs:
        if o <= off < o + ln:
            return va + off - o
    return 0


seen = collections.OrderedDict()
for mo in re.finditer(pat, m):
    i = mo.start()
    a = m.rfind(b'\n', max(0, i - 400), i) + 1
    b = m.find(b'\n', i, i + 400)
    if b < 0:
        b = i + 400
    line = m[a:b]
    if len(line) > 380 or b'\\' in line:
        continue
    try:
        t = line.decode('utf-8').strip()
    except UnicodeDecodeError:
        continue
    if not t or sum(ch.isprintable() or ch == '\t' for ch in t) < len(t):
        continue
    if t not in seen:
        seen[t] = [0, va_of(a)]
    seen[t][0] += 1
print(len(seen), 'distinct lines')
for t, (c, va) in list(seen.items())[:limit]:
    print(('%08X ' % va if show_va else '') + '%3d  %s' % (c, t[:230]))
