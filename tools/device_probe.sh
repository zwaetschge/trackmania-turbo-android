#!/bin/sh
# Probe the running game from where it is: timed pad presses while the screen is recorded
# and the game is profiled; afterwards the frame statistics of that stretch are fetched.
# usage: device_probe.sh <name> <seconds<=45> [<t>:<code>[:<hold_ms>] ...]
#   pad codes: A 96, B 97, X 99, Y 100, START 108, UP 19, DOWN 20, LEFT 21, RIGHT 22, R2 105
#   (the game must have been started with TM_GFX_STATS=30, e.g. by tools/device_menu.sh)
# Result in work/devcap/: <name>.mp4, <name>.threads.txt, <name>.perf.txt (main thread by
# address, for tools/perf_agg.py), <name>.fps.txt (statistics lines of the stretch).
# TM_PROBE_TID=<n-th busiest thread, default 1> selects the thread for <name>.perf.txt.
# Refuses to run when the player touched the device since our last input.
cd "$(dirname "$0")/.."
. tools/env.sh
NAME=$1; SEC=$2; shift 2
D=tools/device.sh; PKG=ch.plum.tmturbo.recomp; U=/sdcard/Android/data/$PKG/files/user
. tools/device_guard.sh
guard_resume
CMD="cd /sdcard/Download; rm -f tmv.mp4; grep -a -c 'gfx stats: [0-9.]* fps' $U/tmrecomp.log > tmn.txt; nohup sh -c 'screenrecord --time-limit $SEC --size 960x540 --bit-rate 8000000 tmv.mp4' >/dev/null 2>&1 &
P=\$(pidof $PKG); nohup run-as $PKG sh -c \"cd /data/data/$PKG && simpleperf record -p \$P -f 1000 --duration $((SEC - 2)) -o perf.data\" >/dev/null 2>&1 & sleep 0.5"
for s in "$@"; do
  t=${s%%:*}; r=${s#*:}; code=${r%%:*}; hold=${r#*:}; [ "$hold" = "$r" ] && hold=120
  CMD="$CMD; ( (sleep $t; input gamepad keyevent --duration $hold $code) & )"
done
CMD="$CMD; sleep $SEC; sleep 1.5; true"
$D sh "$CMD" $(((SEC + 12) * 1000)) >/dev/null
guard_mark
MT=$($D sh "run-as $PKG sh -c 'cd /data/data/$PKG && simpleperf report -i perf.data --sort tid,comm -n 2>/dev/null' | sed -n 6,16p; true" 50000)
echo "$MT" > work/devcap/$NAME.threads.txt
TID=$(echo "$MT" | awk -v n=${TM_PROBE_TID:-1} '$1 ~ /%/ {k++; if (k == n) {print $3; exit}}')
$D sh "cd /sdcard/Download; curl -s -T tmv.mp4 $TM_CAPTURE_URL/$NAME.mp4; rm -f tmv.mp4; run-as $PKG sh -c 'cd /data/data/$PKG && simpleperf report -i perf.data --tids $TID --sort dso,vaddr_in_file -n 2>/dev/null' | curl -s -T - $TM_CAPTURE_URL/$NAME.perf.txt; N=\$(cat tmn.txt); grep -a -E 'gfx stats: [0-9.]* fps|splits|long frame' $U/tmrecomp.log | awk -v n=\$N '/fps \\|/ {k++} k > n' | curl -s -T - $TM_CAPTURE_URL/$NAME.fps.txt; rm -f tmn.txt; true" 55000 >/dev/null
ls -la work/devcap/$NAME.mp4 work/devcap/$NAME.perf.txt work/devcap/$NAME.fps.txt | cut -c24-; head -7 work/devcap/$NAME.threads.txt
