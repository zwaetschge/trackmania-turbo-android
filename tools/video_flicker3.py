"""Flicker that survives fast camera motion: the picture is averaged over an 8x6 grid and
a frame counts when many cells jump away from the mean of their two neighbours in time
(second difference) by more than the cell changes between those neighbours.
usage: video_flicker3.py <video> [luma_step=3] [cell_share=0.25] [t_from t_to]
Prints the frames, a summary, and writes <video>.flicker3.png (prev | frame | next)."""
import subprocess, sys, numpy as np
v = sys.argv[1]; step = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0; share = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
t0 = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0; t1 = float(sys.argv[5]) if len(sys.argv) > 5 else 1e9
W, H, GX, GY = 96, 54, 8, 6
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', v, '-vf', f'scale={W}:{H}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
ts = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'frame=pts_time', '-of', 'csv=p=0', v], capture_output=True, text=True).stdout.split()
n = len(raw) // (W * H)
f = np.frombuffer(raw[:n * W * H], np.uint8).reshape(n, H, W).astype(np.float32)
t = np.array([float(x.strip(',')) for x in ts[:n]] + [0.0] * max(0, n - len(ts)))
g = f[:, :GY * (H // GY), :GX * (W // GX)].reshape(n, GY, H // GY, GX, W // GX).mean((2, 4))     # n x GY x GX cell means
hits = []
for i in range(1, n - 1):
    if t[i] < t0 or t[i] > t1: continue
    z = np.abs(g[i] - (g[i - 1] + g[i + 1]) / 2)
    drift = np.abs(g[i + 1] - g[i - 1])
    bad = (z > step) & (z > drift)
    if bad.mean() >= share: hits.append((i, t[i], float(bad.mean()), float(z[bad].mean()), float((g[i] - (g[i - 1] + g[i + 1]) / 2).mean())))
for i, tt, sh, amp, sign in hits[:40]:
    print(f"  t={tt:7.3f}s frame {i}: {100 * sh:3.0f} % of the cells jump by {amp:4.1f} on average ({'brighter' if sign > 0 else 'darker'})")
sel = int(((t >= t0) & (t <= t1)).sum())
print(f"{sel} frames, {len(hits)} flicker frames ({100.0 * len(hits) / max(1, sel):.1f} %)")
if hits:
    picks = hits[:8]
    vf = "+".join(f"eq(n\\,{i - 1})+eq(n\\,{i})+eq(n\\,{i + 1})" for i, *_ in picks)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', v, '-vf', f"select='{vf}',scale=426:-1,tile=3x{len(picks)}", '-vsync', '0', '-frames:v', '1', v + '.flicker3.png'])
