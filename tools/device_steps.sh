#!/bin/sh
# Run a list of timed input steps on the device with a screenshot after each one,
# upload them through tools/lan_recv.py and build a contact sheet work/<name>.png.
# usage: device_steps.sh <name> <step> [<step>...]
#   step = pad:<keycode>[:hold_ms] | key:<keycode>[:hold_ms] | wait:<seconds> | shot
#   (pad codes: A 96, B 97, X 99, Y 100, START 108, SELECT 109, UP 19, DOWN 20, LEFT 21, RIGHT 22, R2 105, L2 104)
# A screenshot is taken after every pad/key step (0.35 s later) and for every "shot".
cd "$(dirname "$0")/.."
. tools/env.sh
NAME=$1; shift
CMD="mkdir -p /sdcard/Download/tmshots; cd /sdcard/Download/tmshots; rm -f *.png; i=100"
SHOT='i=$((i+1)); screencap -p s$i.png'
for s in "$@"; do
  k=${s%%:*}; r=${s#*:}; code=${r%%:*}; hold=${r#*:}; [ "$hold" = "$r" ] && hold=200
  case $k in
    pad) CMD="$CMD; input gamepad keyevent --duration $hold $code; sleep 0.35; $SHOT" ;;
    key) CMD="$CMD; input keyevent --duration $hold $code; sleep 0.35; $SHOT" ;;
    wait) CMD="$CMD; sleep $code" ;;
    shot) CMD="$CMD; $SHOT" ;;
  esac
done
CMD="$CMD; tar cf - *.png | curl -s -T - $TM_CAPTURE_URL/$NAME.tar; rm -f *.png; cd ..; rmdir tmshots"
tools/device.sh sh "$CMD" 90000 >/dev/null
rm -rf work/devcap/$NAME; mkdir -p work/devcap/$NAME; tar xf work/devcap/$NAME.tar -C work/devcap/$NAME
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$TM_HOST_ROOT:/w" -w /w tm-hostdev:4 sh -c "montage \$(ls work/devcap/$NAME/s*.png | sort -V) -tile 4x -geometry 480x270+2+2 -background '#222' work/$NAME.png 2>&1 | grep -v Fontconfig"
ls -la work/$NAME.png | cut -c20-
