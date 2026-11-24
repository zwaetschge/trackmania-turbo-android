#!/bin/sh
# From the series page (where tools/device_menu.sh stops): enter the series, pick campaign
# track <n> (1..10), load it, start the race and record <seconds> with the accelerator held.
# usage: device_track.sh <n> <name> [seconds=16]
# Result: work/devcap/<name>.mp4 and the sheet work/<name>.png (2 frames per second).
# Guarded: stops when the device sees input that is not ours.
cd "$(dirname "$0")/.."
N=$1; NAME=$2; SEC=${3:-16}
STEPS="pad:96 wait:8"
[ "$N" -gt 5 ] && { STEPS="$STEPS pad:20 wait:0.8"; N=$((N - 5)); }
while [ "$N" -gt 1 ]; do STEPS="$STEPS pad:22 wait:0.8"; N=$((N - 1)); done
tools/device_keys.sh $STEPS pad:96 wait:33 pad:96 wait:1.6 pad:96 wait:1.6 pad:96 wait:1.6 pad:96 wait:0.5 || exit 3
TM_DRIVE_FPS=2 TM_DRIVE_TILE=8x5 tools/device_drive.sh "$NAME" "$SEC" 2>&1 | tail -2
