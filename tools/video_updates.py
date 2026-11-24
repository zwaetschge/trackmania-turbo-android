#!/usr/bin/env python3
"""How often does a screen region really change in a 60 fps device recording?

    tools/video_updates.py <video> <from_s> <seconds> [x y w h (of 960x540)] [threshold=0.3]

Prints one character per recorded frame ('#' = the region differs from the previous frame,
'.' = identical within the threshold, in percent of pixels that changed by more than 10
levels) and the update rate per second. A game that presents 60 new frames per second
gives rows of '#' on anything that animates; an animation that only advances every few
frames shows as '#....#....'.
"""
import subprocess
import sys

import numpy as np

W, H = 960, 540


def main():
    path, t0, dur = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    x, y, w, h = (int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else (0, 0, W, H)
    thr = float(sys.argv[8]) if len(sys.argv) > 8 else 0.3
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-ss', str(t0), '-t', str(dur), '-i', path,
                          '-vf', 'scale=%d:%d,crop=%d:%d:%d:%d' % (W, H, w, h, x, y),
                          '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], stdout=subprocess.PIPE)
    prev = None
    flags = []
    while True:
        b = p.stdout.read(w * h)
        if len(b) < w * h:
            break
        f = np.frombuffer(b, dtype=np.uint8).astype(np.int16)
        if prev is not None:
            flags.append(100.0 * (np.abs(f - prev) > 10).mean() > thr)
        prev = f
    for s in range(0, len(flags), 60):
        seg = flags[s:s + 60]
        print('%6.1f s  %2d updates  %s' % (t0 + s / 60.0, sum(seg), ''.join('#' if v else '.' for v in seg)))
    gaps = []
    last = None
    for i, v in enumerate(flags):
        if v:
            if last is not None:
                gaps.append(i - last)
            last = i
    if gaps:
        g = np.array(gaps)
        print('updates: %d in %.1f s; frames between updates: median %d, p90 %d, max %d' % (
            len(g) + 1, len(flags) / 60.0, np.median(g), np.percentile(g, 90), g.max()))


if __name__ == '__main__':
    main()
