#!/bin/sh
# Frame rate of the game's surface on the device, from SurfaceFlinger's latency
# history of the last ~120 presented frames. usage: device_fps.sh
cd "$(dirname "$0")/.."
tools/device.sh sh 'L=$(dumpsys SurfaceFlinger --list | grep -o "SurfaceView\[ch.plum.tmturbo.recomp[^]]*\](BLAST)#[0-9]*" | head -1); dumpsys SurfaceFlinger --latency "$L"' | python3 -c '
import sys
rows=[l.split() for l in sys.stdin][1:]
t=[int(r[1]) for r in rows if len(r)==3 and r[1]!="0" and int(r[1])<(1<<62)]
if len(t)<3: print("no frame history", len(t)); sys.exit()
d=[(b-a)/1e6 for a,b in zip(t,t[1:]) if b>a]
d.sort()
print("frames %d  avg %.1f fps  median %.1f ms  p90 %.1f ms  max %.1f ms" % (len(t), 1000*len(d)/sum(d), d[len(d)//2], d[int(len(d)*0.9)], d[-1]))'
