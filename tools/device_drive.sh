#!/bin/sh
# Drive a timed input script on the device while recording the screen: the accelerator
# (RT) is held for the whole run, every step presses one more pad button at a given time.
# usage: device_drive.sh [--start|--restart] <name> <seconds> [<t>:<code>:<hold_ms> ...]
#   --start    launch the game first and go to the start of campaign track #01 (~3.5 min)
#   --restart  press B (restart the race) first; use it while a race is running
#   t is in seconds after the recording starts (fractions allowed); pad codes: LEFT 21,
#   RIGHT 22, A 96, B 97, X 99, Y 100, L2 104 (brake), R2 105, START 108.
#   TM_DRIVE_PRE='<adb shell command>' runs right before the recording starts,
#   TM_DRIVE_GAS=0 does not hold the accelerator.
# Result: work/devcap/<name>.mp4 and a contact sheet work/<name>.png (TM_DRIVE_FPS, default 2).
# The start of the race (after "GO") is about 4 s into the recording with --start/--restart.
cd "$(dirname "$0")/.."
. tools/env.sh
D=tools/device.sh; A='input gamepad keyevent --duration 200 96'
MODE=; case "$1" in --start|--restart) MODE=$1; shift ;; esac
NAME=$1; SEC=$2; shift 2
. tools/device_guard.sh
if [ "$MODE" = --start ]; then
  tools/device_to_track.sh "$NAME" >/dev/null || { echo "device in use - not started"; exit 3; }
  $D sh "$A; sleep 25" 60000 >/dev/null
  $D sh "sleep 8; $A; sleep 1.6; $A; sleep 1.6; $A; sleep 1.6; $A; sleep 2" 60000 >/dev/null
fi
[ "$MODE" = --start ] || guard_resume
CMD="cd /sdcard/Download; rm -f tmv.mp4"
[ "$MODE" = --restart ] && CMD="$CMD; input gamepad keyevent --duration 200 97; sleep 1"
[ -n "$TM_DRIVE_PRE" ] && CMD="$CMD; $TM_DRIVE_PRE"
CMD="$CMD; (screenrecord --time-limit $SEC --size 960x540 --bit-rate 8000000 tmv.mp4 &) ; sleep 0.5"
[ "${TM_DRIVE_GAS:-1}" = 1 ] && CMD="$CMD; (input gamepad keyevent --duration $((SEC * 1000)) 105 &)"
for s in "$@"; do
  t=${s%%:*}; r=${s#*:}; code=${r%%:*}; hold=${r#*:}; [ "$hold" = "$r" ] && hold=200
  CMD="$CMD; ( (sleep $t; input gamepad keyevent --duration $hold $code) & )"
done
CMD="$CMD; sleep $SEC; sleep 2; curl -s -T tmv.mp4 $TM_CAPTURE_URL/$NAME.mp4; rm -f tmv.mp4; true"
$D sh "$CMD" $(((SEC + 30) * 1000)) >/dev/null
guard_mark
ffmpeg -loglevel error -y -i work/devcap/$NAME.mp4 -vf "fps=${TM_DRIVE_FPS:-2},scale=320:-1,tile=${TM_DRIVE_TILE:-8x8}" -frames:v 1 work/$NAME.png 2>&1 | tail -2
ls -la work/devcap/$NAME.mp4 work/$NAME.png | cut -c20-
