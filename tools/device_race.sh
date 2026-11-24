#!/bin/sh
# Launch the game on the device and drive it INTO a running race on campaign
# track #01, then hold the accelerator. usage: device_race.sh <tag> [hold_seconds=20]
# (extras via TM_AM_EXTRA). Leaves work/dev-<tag>-race.png.
#
# The pre-race screen needs three separate A presses: after ~5 s without input
# the mode script enters its attract mode ("INSERT COIN"); the first press only
# ends the idle state, the second brings the panel back, the third selects
# "DRIVE ALONE". Presses must be >1 s apart.
cd "$(dirname "$0")/.."
TAG=${1:-r}; HOLD=${2:-20}; D=tools/device.sh; A='input gamepad keyevent --duration 200 96'
tools/device_to_track.sh "$TAG" >/dev/null
$D sh "$A; sleep 25" 60000 >/dev/null          # track grid -> loading (or: wake, if already in the track)
$D sh "sleep 8; $A; sleep 1.6; $A; sleep 1.6; $A; sleep 1.6; $A; sleep 7" 60000 >/dev/null   # wake, panel, select; countdown
$D sh "(input gamepad keyevent --duration $((HOLD * 1000)) 105 &) ; sleep 6" 30000 >/dev/null   # RT = accelerate
$D shot dev-$TAG-race | tail -1
