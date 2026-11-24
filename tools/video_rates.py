#!/usr/bin/env python3
"""Map of how often each part of the screen changes in a 60 fps device recording.

    tools/video_rates.py <video> <from_s> <seconds> [block=40]

Prints a grid (one character per block of the 960x540 picture): changes per second of the
block, as '.' (static), digits 1-9 = up to 6*n per second, '#' = every frame. A video that
is decoded too slowly shows as an area of low digits next to parts that update every frame.
"""
import subprocess
import sys

import numpy as np

W, H = 960, 540


def main():
    path, t0, dur = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    B = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-ss', str(t0), '-t', str(dur), '-i', path,
                          '-vf', 'scale=%d:%d' % (W, H), '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], stdout=subprocess.PIPE)
    bh, bw = H // B, W // B
    counts = np.zeros((bh, bw))
    prev = None
    n = 0
    while True:
        b = p.stdout.read(W * H)
        if len(b) < W * H:
            break
        f = np.frombuffer(b, dtype=np.uint8).reshape(H, W).astype(np.int16)
        if prev is not None:
            d = (np.abs(f - prev) > 6)[:bh * B, :bw * B].reshape(bh, B, bw, B).mean(axis=(1, 3))
            counts += d > 0.02
            n += 1
        prev = f
    rate = counts / max(n, 1) * 60.0
    for r in rate:
        print(''.join('.' if v < 1.5 else ('#' if v > 54 else str(min(9, int(v // 6) + 1))) for v in r))
    print('%d frames; block changes per second: median of moving blocks %.0f' % (n, np.median(rate[rate >= 1.5]) if (rate >= 1.5).any() else 0))


if __name__ == '__main__':
    main()
