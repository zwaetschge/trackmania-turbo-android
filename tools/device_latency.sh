#!/bin/sh
# Real display cadence around a pad press: press <code> (default A = 96), wait <seconds>
# (default 2.1 - SurfaceFlinger keeps about the last 2 s), then read the present times of
# the game's surface. Prints the frame intervals as one character per frame
# ('.' = 16.7 ms, digit n = n refreshes) and the long ones. No recorder running.
# usage: device_latency.sh [code=96] [wait_s=2.1] [pre: '<adb shell command>']
cd "$(dirname "$0")/.."
. tools/device_guard.sh
guard_resume
CODE=${1:-96}; W=${2:-2.1}; PRE=${3:-true}
tools/device.sh sh "$PRE; L=\$(dumpsys SurfaceFlinger --list | grep -o 'SurfaceView\\[ch.plum.tmturbo.recomp[^]]*\\](BLAST)#[0-9]*' | head -1); input gamepad keyevent --duration 100 $CODE; sleep $W; dumpsys SurfaceFlinger --latency \"\$L\"" 40000 | python3 -c '
import sys
rows=[l.split() for l in sys.stdin][1:]
t=[int(r[1]) for r in rows if len(r)==3 and r[1]!="0" and int(r[1])<(1<<62)]
if len(t)<3: print("no frame history", len(t)); sys.exit()
d=[(b-a)/1e6 for a,b in zip(t,t[1:]) if b>a]
s="".join("." if x<25 else str(min(9,int(round(x/16.67)))) for x in d)
print("%d frames over %.2f s: %s" % (len(t), (t[-1]-t[0])/1e9, s))
long=[round(x) for x in d if x>=25]
print("intervals >= 2 refreshes: %d  %s; longest %.0f ms" % (len(long), long[:30], max(d)))'
guard_mark
