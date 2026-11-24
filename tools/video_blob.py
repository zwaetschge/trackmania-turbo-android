#!/usr/bin/env python3
"""Count frames with a white blob (blown-out glow) in device recordings of a static scene.

    tools/video_blob.py <video> [<video> ...]

A frame counts when its share of near-white pixels (luma >= 232) is more than 3 points
above the median of the recording. Prints the count and the positions of the first hits.
"""
import subprocess
import sys

import numpy as np

W, H = 480, 270
fr = []
for path in sys.argv[1:]:
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', path, '-vf', 'scale=%d:%d' % (W, H),
                          '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], stdout=subprocess.PIPE)
    while True:
        b = p.stdout.read(W * H)
        if len(b) < W * H:
            break
        fr.append(100.0 * (np.frombuffer(b, dtype=np.uint8) >= 232).mean())
fr = np.array(fr)
if not len(fr):
    print('no frames')
    sys.exit()
med = np.median(fr)
hit = np.where(fr > med + 3.0)[0]
print('%d frames, %d with a white blob (%.2f %%); white share median %.1f %%, max %.1f %%; first hits at frames %s' % (
    len(fr), len(hit), 100.0 * len(hit) / len(fr), med, fr.max(), hit[:8].tolist()))
