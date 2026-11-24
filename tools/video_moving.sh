#!/bin/sh
# Is the picture still changing late in a recording? prints the mean absolute difference of
# two frames one second apart (default: frames 1500 and 1560); a parked car gives ~0.
# usage: video_moving.sh <video> [frame_a=1500] [frame_b=1560]
A=${2:-1500}; B=${3:-1560}
ffmpeg -loglevel error -i "$1" -vf "select='eq(n,$A)+eq(n,$B)',scale=96:54,format=gray" -vsync 0 -f rawvideo - 2>/dev/null | python3 -c '
import sys
d=sys.stdin.buffer.read(); n=96*54
print(0 if len(d)<2*n else sum(abs(d[i]-d[n+i]) for i in range(n))//n)'
