#!/bin/sh
# Launch the game, load campaign track #01 and leave it idle at the pre-race panel: with a
# saved ghost the attract mode then replays that run with cinematic cameras (jumps, turbo,
# wide views) -- a hands-free heavy scene. usage: device_replay.sh <tag>  (extras via TM_AM_EXTRA)
cd "$(dirname "$0")/.."
tools/device_to_track.sh "${1:-rp}" >/dev/null
tools/device.sh sh 'input gamepad keyevent --duration 200 96; sleep 26; true' 60000 >/dev/null
tools/device.sh sh 'sleep 12; dumpsys window | grep -a mCurrentFocus | head -1; true' 30000 2>/dev/null | sed 's/.* u0 //; s/}//'
