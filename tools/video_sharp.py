"""Does the picture's sharpness alternate from frame to frame? (Motion blur or another
full-screen effect being applied in some frames and not in others.)
usage: video_sharp.py <video> [rel_jump=0.18] [t_from t_to]
Sharpness = mean absolute horizontal+vertical gradient on a 480x270 grey picture. A frame
counts when its sharpness differs from BOTH neighbours by more than rel_jump while the two
neighbours agree with each other (within half of that). Prints the share of such frames
and where they cluster (per second)."""
import subprocess, sys, numpy as np, collections
v = sys.argv[1]; rel = float(sys.argv[2]) if len(sys.argv) > 2 else 0.18
t0 = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0; t1 = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
W, H = 480, 270
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', v, '-vf', f'scale={W}:{H}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
ts = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'frame=pts_time', '-of', 'csv=p=0', v], capture_output=True, text=True).stdout.split()
n = len(raw) // (W * H)
f = np.frombuffer(raw[:n * W * H], np.uint8).reshape(n, H, W).astype(np.float32)
t = np.array([float(x.strip(',')) for x in ts[:n]] + [0.0] * max(0, n - len(ts)))
body = f[:, 30:240, :]                                     # leave out the HUD rows
s = np.abs(np.diff(body, axis=2)).mean((1, 2)) + np.abs(np.diff(body, axis=1)).mean((1, 2))
dup = np.array([False] + [bool(np.abs(f[i] - f[i - 1]).mean() < 0.05) for i in range(1, n)])   # repeated frames
hits = []; per_sec = collections.Counter()
for i in range(1, n - 1):
    if t[i] < t0 or t[i] > t1: continue
    a, b, c = s[i - 1], s[i], s[i + 1]
    m = (a + c) / 2
    if m > 1 and abs(b - a) > rel * m and abs(b - c) > rel * m and (b - a) * (b - c) > 0 and abs(a - c) < rel * m / 2:
        hits.append(i); per_sec[int(t[i])] += 1
sel = int(((t >= t0) & (t <= t1)).sum())
print(f"{sel} frames, {int(dup.sum())} repeated; sharpness alternates in {len(hits)} frames ({100.0 * len(hits) / max(1, sel):.1f} %)")
if hits: print("  per second:", ' '.join(f"{k}s:{c}" for k, c in sorted(per_sec.items())))
