#!/bin/sh
# Take N on-device screenshots (default 8, ~2.5 s apart), upload them through
# tools/lan_recv.py (port 8766) and build a contact sheet work/<name>.png.
# usage: device_seq.sh <name> [count] [sleep_s] ['<adb command to run first>']
cd "$(dirname "$0")/.."
. tools/env.sh
NAME=${1:-seq}; N=${2:-8}; S=${3:-2}; PRE=${4:-true}
LIST=$(seq 1 $N | sed 's/^/s/; s/$/.png/' | tr '\n' ' ')
tools/device.sh sh "mkdir -p /sdcard/Download/tmshots; cd /sdcard/Download/tmshots; $PRE; for i in \$(seq 1 $N); do screencap -p s\$i.png; sleep $S; done; tar cf - $LIST | curl -s -T - $TM_CAPTURE_URL/$NAME.tar; rm -f $LIST; cd ..; rmdir tmshots" 90000 >/dev/null
rm -rf work/devcap/$NAME; mkdir -p work/devcap/$NAME; tar xf work/devcap/$NAME.tar -C work/devcap/$NAME
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$TM_HOST_ROOT:/w" -w /w tm-hostdev:4 sh -c "montage \$(ls work/devcap/$NAME/s*.png | sort -V) -tile 4x -geometry 400x225+2+2 -background '#222' work/$NAME.png 2>&1 | grep -v Fontconfig"
ls -la work/$NAME.png | cut -c20-
