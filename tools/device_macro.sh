#!/bin/sh
# Tune a pad macro: set it, restart the race (B), record, and build a sheet of a time window.
# usage: device_macro.sh <name> '<macro>' [seconds=16] [sheet_from=6] [fps=4] [tile=8x5]
#   macro as in runtime/src/input.c ("GAS:500:30000,LEFT:7050:7750"); times in ms after B,
#   GO is at about 3100 ms. The recording starts ~1.3 s after B.
cd "$(dirname "$0")/.."
NAME=$1; MACRO=$2; SEC=${3:-16}; FROM=${4:-6}; FPS=${5:-4}; TILE=${6:-8x5}
tools/device.sh sh "setprop debug.tm.padmacro '$MACRO'; true" >/dev/null
TM_DRIVE_GAS=0 tools/device_drive.sh --restart "$NAME" "$SEC" >/dev/null 2>&1
ffmpeg -loglevel error -y -ss "$FROM" -i work/devcap/$NAME.mp4 -vf "fps=$FPS,scale=240:-1,tile=$TILE" -frames:v 1 work/$NAME.png
ls -la work/$NAME.png | cut -c24-
