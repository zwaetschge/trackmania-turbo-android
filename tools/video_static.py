"""How much does a picture that should stand still change from frame to frame?
usage: video_static.py <video> <t_from> <t_to> [out.png]
For the time window (static camera, e.g. the countdown) prints, on a 32x18 grid, how many
cells change by more than 2 luma levels between consecutive frames, and writes a heat map
(mean absolute frame-to-frame change per cell, amplified) next to the first frame."""
import subprocess, sys, numpy as np
v, t0, t1 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
out = sys.argv[4] if len(sys.argv) > 4 else v + '.static.png'
W, H = 320, 180
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-ss', str(t0), '-t', str(t1 - t0), '-i', v, '-vf', f'scale={W}:{H}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
n = len(raw) // (W * H)
f = np.frombuffer(raw[:n * W * H], np.uint8).reshape(n, H, W).astype(np.float32)
d = np.abs(np.diff(f, axis=0))                       # n-1 x H x W
cell = d.reshape(n - 1, 18, 10, 32, 10).mean((2, 4))  # per-cell change per frame pair
act = cell > 2.0
print(f"{n} frames; cells changing by >2 levels: mean {100 * act.mean():.1f} % per frame, worst frame {100 * act.mean((1, 2)).max():.0f} %")
heat = d.mean(0)                                     # H x W
rows = np.argsort(-cell.mean(0).ravel())[:6]
for r in rows: print(f"  cell x {r % 32 * 30:4d} y {r // 32 * 30:4d} (960x540 px): mean change {cell.mean(0).ravel()[r]:.1f}, max {cell.max(0).ravel()[r]:.1f}")
img = np.concatenate([f[0], np.clip(heat * 12, 0, 255)], axis=1).astype(np.uint8)
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'gray', '-s', f'{2 * W}x{H}', '-i', '-', '-vf', 'scale=1280:-1:flags=neighbor', '-frames:v', '1', out], input=img.tobytes())
