#!/usr/bin/env python3
"""Name game shaders from the hashes the bridge's float watch prints.

    tools/shader_hash.py <hash8> [<hash8> ...]      look hashes up
    tools/shader_hash.py --build                    (re)build work/shader_hashes.txt

The hash is FNV-1a (32 bit) over the DXBC blob, as in fwatch_draw (gfx/d3d11_bridge.cpp).
"""
import glob
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work')
TABLE = os.path.join(ROOT, 'shader_hashes.txt')


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def build():
    with open(TABLE, 'w') as out:
        for d in ('shaders_sm5', 'shaders_sm4'):
            for p in sorted(glob.glob(os.path.join(ROOT, d, '*.dxbc'))):
                data = open(p, 'rb').read()
                out.write('%08X %6d %s/%s\n' % (fnv(data), len(data), d, os.path.basename(p)))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--build' or not os.path.exists(TABLE):
        build()
    want = {a.upper() for a in sys.argv[1:] if not a.startswith('--')}
    if not want:
        return
    for line in open(TABLE):
        if line[:8] in want:
            print(line.rstrip())


if __name__ == '__main__':
    main()
