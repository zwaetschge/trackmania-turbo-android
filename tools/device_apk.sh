#!/bin/sh
# Sync + build the debug APK and install it on the device (waits for the build).
cd "$(dirname "$0")/.."
. tools/env.sh
tools/device.sh stop >/dev/null 2>&1
tools/device.sh apk 2>&1 | grep -E '"engine"|rror' | head -2
for i in $(seq 1 50); do
  st=$(node /app/scripts/mcp-call.mjs call android-builder android_get_project '{"projectId":"'"$TM_BUILDER_PROJ"'"}' 2>&1 | python3 -c "
import sys,json
t=sys.stdin.read(); i=t.find('{'); d=json.loads(t[i:]); print(d.get('status'))")
  case "$st" in building*) sleep 12;; *) echo "build: $st"; break;; esac
done
tools/device.sh install 2>&1 | grep -E "Success|rror" | head -2
