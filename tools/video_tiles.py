#!/usr/bin/env python3
"""Find frames with the "tile lattice" glitch in a 960x540 device recording.

The glitch replaces 32x32-pixel screen tiles (16x16 in a 960x540 recording) with flat
colours, on a regular lattice. A frame is scored by the number of tiles that are flat
(low deviation inside) AND differ strongly from at least two of their four neighbours
along the whole shared edge; smooth sky or compression blocks do not score because
their neighbours are similar.

usage: video_tiles.py <video> [min_tiles=12] [dump_dir]
"""
import subprocess
import sys

import numpy as np

W, H, T = 960, 540, 16


def frames(path):
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', path, '-vf', 'scale=%d:%d' % (W, H),
                          '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], stdout=subprocess.PIPE)
    n = 0
    while True:
        b = p.stdout.read(W * H)
        if len(b) < W * H:
            break
        yield n, np.frombuffer(b, dtype=np.uint8).reshape(H, W).astype(np.float32)
        n += 1


def score(g):
    th, tw = H // T, W // T
    t = g[:th * T, :tw * T].reshape(th, T, tw, T).transpose(0, 2, 1, 3)       # th, tw, T, T
    inner = t[:, :, 2:-2, 2:-2].reshape(th, tw, -1)
    mean = inner.mean(axis=2)
    flat = inner.std(axis=2) < 1.6
    # a neighbour "differs" when its edge strip is far from this tile's mean along the whole edge
    diff = np.zeros((th, tw), dtype=np.int32)
    left = np.abs(t[:, :-1, :, -2].mean(axis=2) - mean[:, 1:]) > 14        # strip of the left neighbour
    lmin = np.abs(t[:, :-1, :, -2] - mean[:, 1:, None]).min(axis=2) > 8
    diff[:, 1:] += (left & lmin)
    right = np.abs(t[:, 1:, :, 1].mean(axis=2) - mean[:, :-1]) > 14
    rmin = np.abs(t[:, 1:, :, 1] - mean[:, :-1, None]).min(axis=2) > 8
    diff[:, :-1] += (right & rmin)
    up = np.abs(t[:-1, :, -2, :].mean(axis=2) - mean[1:, :]) > 14
    umin = np.abs(t[:-1, :, -2, :] - mean[1:, :, None]).min(axis=2) > 8
    diff[1:, :] += (up & umin)
    down = np.abs(t[1:, :, 1, :].mean(axis=2) - mean[:-1, :]) > 14
    dmin = np.abs(t[1:, :, 1, :] - mean[:-1, :, None]).min(axis=2) > 8
    diff[:-1, :] += (down & dmin)
    hit = flat & (diff >= 3)
    return int(hit.sum()), int(flat.sum())


def main():
    path = sys.argv[1]
    minimum = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    dump = sys.argv[3] if len(sys.argv) > 3 else None
    hits = []
    total = 0
    for n, g in frames(path):
        total += 1
        s, f = score(g)
        if s >= minimum or f > 1500:
            hits.append((n, s, f))
    print('%s: %d frames, %d with >= %d lattice tiles or mostly flat' % (path, total, len(hits), minimum))
    run = []
    for h in hits:
        if run and h[0] != run[-1][0] + 1:
            print('  frames %d-%d (%.2f-%.2f s): max %d lattice tiles, max %d flat tiles' % (
                run[0][0], run[-1][0], run[0][0] / 60.0, run[-1][0] / 60.0, max(r[1] for r in run), max(r[2] for r in run)))
            run = []
        run.append(h)
    if run:
        print('  frames %d-%d (%.2f-%.2f s): max %d lattice tiles, max %d flat tiles' % (
            run[0][0], run[-1][0], run[0][0] / 60.0, run[-1][0] / 60.0, max(r[1] for r in run), max(r[2] for r in run)))
    if dump and hits:
        import os
        os.makedirs(dump, exist_ok=True)
        best = sorted(hits, key=lambda h: -h[1])[:4]
        for n, s, f in best:
            subprocess.call(['ffmpeg', '-loglevel', 'error', '-y', '-i', path, '-vf', 'select=eq(n\\,%d)' % n,
                             '-frames:v', '1', os.path.join(dump, 'tiles-%05d.png' % n)])


if __name__ == '__main__':
    main()
