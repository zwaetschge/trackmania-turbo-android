#!/bin/sh
# Measure the over-bright glow disc at the start of campaign track #01 for one setting of
# the render sync diagnostic. The game must already be in the race (tools/device_race.sh).
# usage: device_disc.sh '<mode> [lo hi]' [name]     (see TM_GFX_SYNC in gfx/d3d11_bridge.cpp)
# Sets debug.tm.gfxsync, restarts the race (B), records the countdown and prints the share
# of frames in which the region around the start lights is saturated.
cd "$(dirname "$0")/.."
. tools/env.sh
SET=${1:-0}; NAME=${2:-disc}
. tools/device_guard.sh
guard_resume
tools/device.sh sh "setprop debug.tm.gfxsync '$SET'; cd /sdcard/Download; rm -f tmv.mp4; sleep 0.4; (screenrecord --time-limit 6 --size 960x540 --bit-rate 6000000 tmv.mp4 &) ; sleep 0.5; input gamepad keyevent --duration 200 97; sleep 6; sleep 1.5; curl -s -T tmv.mp4 $TM_CAPTURE_URL/$NAME.mp4; rm -f tmv.mp4; true" 60000 >/dev/null
FOCUS=$(tools/device.sh sh 'dumpsys window | grep -a mCurrentFocus | head -1; true' 20000 2>/dev/null)
case "$FOCUS" in *ch.plum.tmturbo.recomp*) ;; *) echo "INVALID: the game is not in the foreground ($FOCUS) - the device is shared, repeat the run"; exit 1;; esac
echo "sync '$SET': $(.venv/bin/python tools/video_region.py work/devcap/$NAME.mp4 400 20 160 130 190 1.5 5.5)"
guard_mark
