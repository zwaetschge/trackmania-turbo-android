#!/usr/bin/env python3
"""How long does a menu transition take in a device recording?

    tools/video_transition.py <video> <press_s> [window_s=12] [x y w h (of 960x540)]

Looks at the region (default: the billboard, 150,60 660x380) from the press on and prints
when the picture starts to change, when it stops changing for good (0.5 s without a change
of more than 1.5 % of the pixels) and the time in between.
"""
import subprocess
import sys

import numpy as np

W, H = 960, 540
path, press = sys.argv[1], float(sys.argv[2])
win = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
x, y, w, h = (int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else (150, 60, 660, 380)
p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-ss', str(press), '-t', str(win), '-i', path,
                      '-vf', 'scale=%d:%d,crop=%d:%d:%d:%d' % (W, H, w, h, x, y), '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                     stdout=subprocess.PIPE)
prev = None
ch = []
while True:
    b = p.stdout.read(w * h)
    if len(b) < w * h:
        break
    f = np.frombuffer(b, dtype=np.uint8).astype(np.int16)
    if prev is not None:
        ch.append(100.0 * (np.abs(f - prev) > 14).mean())
    prev = f
ch = np.array(ch)
big = ch > 1.5
if not big.any():
    print('no change in %.0f s' % win)
    sys.exit()
first = int(np.argmax(big))
last = first
quiet = 0
for i in range(first, len(big)):
    if big[i]:
        last = i
        quiet = 0
    else:
        quiet += 1
        if quiet >= 30:
            break
print('%s: starts %.2f s after the press, settles %.2f s after it: %.2f s of transition (%d frames, %d of them changed)' % (
    path.split('/')[-1], first / 60.0, (last + 1) / 60.0, (last + 1 - first) / 60.0, last + 1 - first, int(big[first:last + 1].sum())))
