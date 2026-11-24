"""Find single-frame flicker in a screen recording, also when only part of the picture
changes: a frame that differs from BOTH neighbours much more than the neighbours differ
from each other. usage: video_flicker2.py <video> [ratio=2.5] [min_diff=4] [t_from t_to]
Prints the suspicious frames (time, |b-a|, |b-c|, |a-c| as mean absolute luma on a
96x54 grid, share of cells that changed) and writes <video>.flicker.png with the first
eight triples (previous | frame | next)."""
import subprocess, sys, numpy as np
v = sys.argv[1]; ratio = float(sys.argv[2]) if len(sys.argv) > 2 else 2.5; mind = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
t0 = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0; t1 = float(sys.argv[5]) if len(sys.argv) > 5 else 1e9
W, H = 96, 54
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', v, '-vf', f'scale={W}:{H}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
n = len(raw) // (W * H)
f = np.frombuffer(raw[:n * W * H], np.uint8).reshape(n, H, W).astype(np.float32)
fps_txt = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=avg_frame_rate', '-of', 'csv=p=0', v], capture_output=True, text=True).stdout.strip()
num, den = (fps_txt.split('/') + ['1'])[:2]; fps = float(num) / float(den or 1)
hits = []
for i in range(1, n - 1):
    t = i / fps
    if t < t0 or t > t1: continue
    a, b, c = f[i - 1], f[i], f[i + 1]
    dab, dbc, dac = np.abs(b - a).mean(), np.abs(b - c).mean(), np.abs(a - c).mean()
    if min(dab, dbc) > mind and min(dab, dbc) > ratio * (dac + 0.5):
        cells = float(((np.abs(b - a) > 12) & (np.abs(b - c) > 12)).mean())
        hits.append((i, t, dab, dbc, dac, cells))
for i, t, dab, dbc, dac, cells in hits[:40]:
    print(f"  t={t:7.3f}s frame {i}: |b-a| {dab:5.1f} |b-c| {dbc:5.1f} |a-c| {dac:5.1f}  changed cells {100 * cells:3.0f} %")
sel = [x for x in range(n) if t0 <= x / fps <= t1]
print(f"{len(sel)} frames ({fps:.0f} fps), {len(hits)} flicker frames ({100.0 * len(hits) / max(1, len(sel)):.1f} %)")
if hits:
    picks = hits[:8]
    vf = "+".join(f"eq(n\\,{i - 1})+eq(n\\,{i})+eq(n\\,{i + 1})" for i, *_ in picks)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', v, '-vf', f"select='{vf}',scale=426:-1,tile=3x{len(picks)}", '-frames:v', '1', v + '.flicker.png'])
