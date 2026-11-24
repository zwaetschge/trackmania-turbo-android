#!/bin/sh
# How long does My Tracks take to load? Launch (extras via TM_AM_EXTRA), walk to TRACKBUILDER,
# press MANAGE YOUR TRACKS and take a screenshot every 3 s until the list replaces LOADING.
# usage: device_mytracks_time.sh <name>   prints the seconds from the press to the list
cd "$(dirname "$0")/.."
NAME=${1:-mtt}
. tools/device_guard.sh
guard_start ${TM_GUARD_IDLE:-600}
tools/device.sh run >/dev/null
guard_mark
tools/device_keys.sh wait:41 pad:96 wait:7 pad:96 wait:13 pad:96 wait:12 pad:19 wait:1 pad:19 wait:1.5 pad:96 wait:6 pad:20 wait:1 || exit 3
tools/device_keys.sh pad:96 || exit 3
T0=$(date +%s)
k=0
while [ $k -lt 60 ]; do
  tools/device.sh shot $NAME-t >/dev/null
  # LOADING is white text in the middle of the dark list area; the list brings thumbnails
  S=$(ffmpeg -loglevel error -i work/$NAME-t.png -vf "scale=480:-1,crop=280:120:150:120,format=gray" -f rawvideo - | python3 -c '
import sys; d=sys.stdin.buffer.read(); n=len(d)
print(sum(1 for b in d if 60 < b < 200) * 100 // max(n,1))')
  if [ "$S" -ge 25 ]; then echo "list after $(( $(date +%s) - T0 )) s (mid-tone share $S %)"; cp work/$NAME-t.png work/$NAME.png; exit 0; fi
  sleep 3; k=$((k+1))
done
echo "still loading after $(( $(date +%s) - T0 )) s"
