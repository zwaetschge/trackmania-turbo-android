#!/bin/sh
# Launch the game, walk to the main menu and then through CAMPAIGN -> SOLO CAMPAIGN -> track
# grid while recording the screen and profiling the game; shows how the menu transitions run.
# usage: device_menu.sh <name>     (extras via TM_AM_EXTRA; TM_GFX_STATS=30 is always set)
# Result: work/devcap/<name>.mp4 (from 68 s after launch, 40 s), <name>.stats.txt (the
# bridge's statistics lines), <name>.threads.txt and <name>.perf.txt (simpleperf: share per
# thread; main-thread samples by address for tools/perf_agg.py), <name>.keys.txt.
# Stops at the track grid. Refuses to run / stops pressing when the player uses the device.
cd "$(dirname "$0")/.."
. tools/env.sh
NAME=$1; D=tools/device.sh; PKG=ch.plum.tmturbo.recomp
. tools/device_guard.sh
guard_start ${TM_GUARD_IDLE:-600}
ENVS="TM_GFX_STATS=30${TM_MENU_ENV:+;$TM_MENU_ENV}"
TM_AM_EXTRA="--es env \"$ENVS\" ${TM_AM_EXTRA:-}" $D run >/dev/null
T0=$(date +%s.%N); _guard_last=$(date +%s); _guard_grace=25
: > work/devcap/$NAME.keys.txt
now() { echo "$(date +%s.%N) - $T0" | bc; }
press() { guard_press gamepad keyevent --duration 200 96; echo "$(now) A $1" >> work/devcap/$NAME.keys.txt; }
wait_until() { while [ "$(echo "$(now) < $1" | bc)" = 1 ]; do sleep 0.3; done; }
wait_until 40;  press "skip intro video"
wait_until 47;  press "title screen"
wait_until 60;  press "offline notice"
wait_until 66
$D sh "cd /sdcard/Download; rm -f tmv.mp4; nohup sh -c 'screenrecord --time-limit 40 --size 960x540 --bit-rate 8000000 tmv.mp4' >/dev/null 2>&1 &
P=\$(pidof $PKG); nohup run-as $PKG sh -c \"cd /data/data/$PKG && simpleperf record -p \$P -f 1000 --duration 36 -o perf.data\" >/dev/null 2>&1 & sleep 0.3; true" >/dev/null
echo "$(now) recording starts" >> work/devcap/$NAME.keys.txt
wait_until 76;  press "main menu: campaign"
wait_until 86;  press "solo campaign"
wait_until 112
MT=$($D sh "run-as $PKG sh -c 'cd /data/data/$PKG && simpleperf report -i perf.data --sort tid,comm -n 2>/dev/null' | sed -n 6,16p; true" 50000)
echo "$MT" > work/devcap/$NAME.threads.txt
TID=$(echo "$MT" | awk '$1 ~ /%/ {print $3; exit}')
$D sh "cd /sdcard/Download; curl -s -T tmv.mp4 $TM_CAPTURE_URL/$NAME.mp4; rm -f tmv.mp4; run-as $PKG sh -c 'cd /data/data/$PKG && simpleperf report -i perf.data --tids $TID --sort dso,vaddr_in_file -n 2>/dev/null' | curl -s -T - $TM_CAPTURE_URL/$NAME.perf.txt; logcat -d -s TMRecomp:V | grep -a 'gfx stats' | curl -s -T - $TM_CAPTURE_URL/$NAME.stats.txt; true" 55000 >/dev/null
ls -la work/devcap/$NAME.mp4 work/devcap/$NAME.perf.txt work/devcap/$NAME.stats.txt | cut -c24-; cat work/devcap/$NAME.keys.txt; head -8 work/devcap/$NAME.threads.txt
