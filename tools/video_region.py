"""Share of frames in which a region of a recording is brighter than a threshold.
usage: video_region.py <video> <x> <y> <w> <h> [luma_threshold=190] [t_from t_to]
(ffmpeg crop + signalstats YAVG; used to detect the over-bright glow disc automatically)"""
import subprocess, sys, re
v = sys.argv[1]; x, y, w, h = sys.argv[2:6]
thr = float(sys.argv[6]) if len(sys.argv) > 6 else 190.0
t0 = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0; t1 = float(sys.argv[8]) if len(sys.argv) > 8 else 1e9
out = subprocess.run(['ffmpeg', '-hide_banner', '-i', v, '-vf', f'crop={w}:{h}:{x}:{y},signalstats,metadata=print:key=lavfi.signalstats.YAVG', '-f', 'null', '-'],
                     capture_output=True, text=True).stderr
ts = [float(a) for a in re.findall(r'pts_time:([0-9.]+)', out)]; ys = [float(a) for a in re.findall(r'YAVG=([0-9.]+)', out)]
sel = [yv for t, yv in zip(ts, ys) if t0 <= t <= t1]
hot = sum(1 for yv in sel if yv > thr)
print(f"{len(sel)} frames, {hot} bright ({100.0 * hot / max(1, len(sel)):.0f} %), region luma min {min(sel):.0f} median {sorted(sel)[len(sel)//2]:.0f} max {max(sel):.0f}")
