"""Find flicker in a screen recording: frames whose mean brightness jumps away from
both neighbours. usage: video_flicker.py <video> [threshold=6]
Prints time, mean luma and the jump for each suspicious frame (ffmpeg signalstats)."""
import subprocess, sys, re
video = sys.argv[1]; thr = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
out = subprocess.run(['ffmpeg', '-hide_banner', '-i', video, '-vf', 'signalstats,metadata=print:key=lavfi.signalstats.YAVG', '-f', 'null', '-'],
                     capture_output=True, text=True).stderr
ts = [float(x) for x in re.findall(r'pts_time:([0-9.]+)', out)]
ys = [float(x) for x in re.findall(r'YAVG=([0-9.]+)', out)]
n = min(len(ts), len(ys)); hits = 0
for i in range(1, n - 1):
    a, b, c = ys[i - 1], ys[i], ys[i + 1]
    if abs(b - a) > thr and abs(b - c) > thr and (b - a) * (b - c) > 0:   # a one-frame spike
        hits += 1
        print(f"  t={ts[i]:7.3f}s  luma {b:6.1f}  (neighbours {a:.1f} / {c:.1f})")
print(f"{n} frames, {hits} one-frame brightness spikes over {thr}")
