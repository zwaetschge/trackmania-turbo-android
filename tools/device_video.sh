#!/bin/sh
# Record the device screen while running an adb shell command, upload the video
# and build a contact sheet (work/<name>.png, 5 fps by default).
# usage: device_video.sh <name> <seconds> '<adb shell command>' [fps=5] [tile=6x]
cd "$(dirname "$0")/.."
. tools/env.sh
NAME=$1; SEC=$2; CMD=$3; FPS=${4:-5}; TILE=${5:-6x}
tools/device.sh sh "cd /sdcard/Download; rm -f tmv.mp4; (screenrecord --time-limit $SEC --size 960x540 --bit-rate 6000000 tmv.mp4 &) ; sleep 0.5; $CMD; sleep $SEC; sleep 1.5; curl -s -T tmv.mp4 $TM_CAPTURE_URL/$NAME.mp4; rm -f tmv.mp4" 90000 >/dev/null
rm -rf work/devcap/$NAME; mkdir -p work/devcap/$NAME
ffmpeg -loglevel error -y -i work/devcap/$NAME.mp4 -vf "fps=$FPS,scale=320:-1,tile=$TILE" -frames:v 1 work/$NAME.png 2>&1 | tail -2
ls -la work/$NAME.png | cut -c20-
