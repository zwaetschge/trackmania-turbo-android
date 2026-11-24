#!/bin/sh
# Hands-free repro of the long jump on campaign track #01 with the race camera: restart
# the race with B; the runtime's pad macro (runtime/src/input.c, "debug.tm.padmacro")
# holds the accelerator and steers through the first three bends, timed from the B press.
# The car reaches the "BIG JUMP" ramp about 23 s after B and is in the air over the canyon
# until the recording ends.
# usage: device_jump.sh <name> [seconds=28]
#   (a race must be running: "TM_DRIVE_GAS=0 tools/device_drive.sh --start x 4" once)
#   TM_DRIVE_PRE as in device_drive.sh; TM_JUMP_MACRO replaces the steering steps.
# Result: work/devcap/<name>.mp4, sheet work/<name>.png (from 21 s), and on stdout the
# number of frames with the tile-lattice glitch (tools/video_tiles.py).
cd "$(dirname "$0")/.."
NAME=$1; SEC=${2:-28}
STEER=${TM_JUMP_MACRO:-LEFT:7400:7650,RIGHT:8300:8700,LEFT:9500:9800}
tools/device.sh sh "setprop debug.tm.padmacro 'GAS:500:$((SEC * 1000 + 4000)),$STEER'; true" >/dev/null
TM_DRIVE_GAS=0 tools/device_drive.sh --restart "$NAME" "$SEC" >/dev/null 2>&1
ffmpeg -loglevel error -y -ss 21 -i work/devcap/$NAME.mp4 -vf "fps=3,scale=320:-1,tile=7x3" -frames:v 1 work/$NAME.png
python3 -c 'import numpy' 2>/dev/null && PY=python3 || PY=.venv/bin/python
echo "moving at the end: $(tools/video_moving.sh work/devcap/$NAME.mp4 $((SEC * 60 - 120)) $((SEC * 60 - 60)))"
$PY tools/video_tiles.py work/devcap/$NAME.mp4 8 | head -${TM_JUMP_LINES:-1}
