#!/bin/sh
# CPU profile of the running game on the device (simpleperf through run-as).
#   device_perf.sh record [seconds=4] [extra simpleperf record args, e.g. "--call-graph fp"]
#   device_perf.sh threads                 share per thread
#   device_perf.sh top [tid] [n=30]        hottest symbols (of one thread)
#   device_perf.sh callers <tid> [n=60]    call-graph report of one thread (needs a --call-graph record)
cd "$(dirname "$0")/.."
PKG=ch.plum.tmturbo.recomp
ra() { tools/device.sh sh "run-as $PKG sh -c \"cd /data/data/$PKG && $1\"" 60000 | sed 's#/data/app/[^ ]*/lib/arm64/##; s#/apex/com.android.runtime/##'; }
case "$1" in
  record) tools/device.sh sh "P=\$(pidof $PKG); run-as $PKG sh -c \"cd /data/data/$PKG && simpleperf record -p \$P -f 1000 --duration ${2:-4} ${3:-} -o perf.data 2>&1 | tail -2\"" 60000 ;;
  threads) ra "simpleperf report -i perf.data --sort tid -n 2>/dev/null | sed -n 6,20p" ;;
  top) if [ -n "$2" ]; then ra "simpleperf report -i perf.data --tids $2 --sort dso,symbol -n 2>/dev/null | sed -n 7,$((7 + ${3:-30}))p"; else ra "simpleperf report -i perf.data --sort dso,symbol -n 2>/dev/null | sed -n 7,$((7 + ${3:-30}))p"; fi | sed 's/  */ /g' | cut -c1-170 ;;
  callers) ra "simpleperf report -i perf.data --tids $2 --children --sort symbol -n 2>/dev/null | sed -n 7,$((7 + ${3:-60}))p" | sed 's/  */ /g' | cut -c1-170 ;;
  *) sed -n 2,7p "$0" ;;
esac
