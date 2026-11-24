#!/bin/sh
# Launch the game on the device and drive it to the first campaign track (usually the track
# grid; see device_race.sh for the presses that actually start the race) with
# gamepad A presses (200 ms each). usage: device_to_track.sh <tag>   (extras via TM_AM_EXTRA)
# Stops at the pre-race screen without further input. Screenshot: work/dev-<tag>-track.png.
cd "$(dirname "$0")/.."
TAG=${1:-t}; D=tools/device.sh; A='input gamepad keyevent --duration 200 96'
. tools/device_guard.sh
guard_start ${TM_GUARD_IDLE:-600} || exit 3      # never take the device from the player
$D run | tail -1
$D sh 'sleep 28' 60000 >/dev/null
$D sh "sleep 12; $A; sleep 7; $A; sleep 8" 60000 >/dev/null     # skip intro video, title screen
$D sh "sleep 5; $A; sleep 14" 60000 >/dev/null                   # offline notice -> main menu
$D sh "$A; sleep 10; $A; sleep 12" 60000 >/dev/null              # campaign, solo campaign -> track grid
$D sh "$A; sleep 25" 60000 >/dev/null                            # track #01 -> loading
$D sh 'sleep 20' 60000 >/dev/null
$D shot dev-$TAG-track | tail -1
guard_mark
