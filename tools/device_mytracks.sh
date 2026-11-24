#!/bin/sh
# Launch the game (extras via TM_AM_EXTRA) and open TRACKBUILDER -> MANAGE YOUR TRACKS
# ("My Tracks") with guarded presses; screenshot work/<name>.png. usage: device_mytracks.sh <name>
cd "$(dirname "$0")/.."
NAME=${1:-mytracks}
. tools/device_guard.sh
guard_start ${TM_GUARD_IDLE:-600}
tools/device.sh run >/dev/null
guard_mark
tools/device_keys.sh wait:41 pad:96 wait:7 pad:96 wait:13 pad:96 wait:12 pad:19 wait:1 pad:19 wait:1.5 pad:96 wait:6 pad:20 wait:1 pad:96 wait:9 shot:$NAME || exit 3
ls -la work/$NAME.png | cut -c24-
