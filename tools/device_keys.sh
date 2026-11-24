#!/bin/sh
# Guarded input steps on the running game (our own session): each step is checked against
# the player's activity first (tools/device_guard.sh).
# usage: device_keys.sh <step> [<step> ...]
#   pad:<code>[:hold_ms]   gamepad button (A 96, B 97, X 99, Y 100, START 108, UP 19, DOWN 20, LEFT 21, RIGHT 22)
#   key:<code>             keyboard key (F8 138 ... F10 140, F11 141 = memory dump, F12 142)
#   tap:<x>:<y>            touch tap at screen pixels (the game sees the mouse cursor and a left click)
#   wait:<seconds>         shot:<name>  (screenshot to work/<name>.png)
cd "$(dirname "$0")/.."
. tools/device_guard.sh
guard_resume
for s in "$@"; do
  k=${s%%:*}; r=${s#*:}; a=${r%%:*}; b=${r#*:}; [ "$b" = "$r" ] && b=150
  case $k in
    pad) guard_press gamepad keyevent --duration $b $a ;;
    key) guard_press keyevent $a ;;
    tap) guard_press tap $a $b ;;                  # tap:<x>:<y> (screen pixels; the game sees the cursor + a click)
    move) guard_press swipe $a $b $a $b 400 ;;      # move:<x>:<y> a long press without moving
    wait) sleep $a ;;
    shot) tools/device.sh shot $a >/dev/null; ffmpeg -loglevel error -y -i work/$a.png -vf scale=800:-1 work/$a-s.png ;;
  esac
done
