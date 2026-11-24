#!/bin/sh
# Device loop helpers for a test device through the android-builder MCP (settings: tools/env.sh).
#   tools/device.sh apk        sync android/ to the builder workspace, build the debug APK, install it
#   tools/device.sh run [sec]  clear logcat, start the game (autostart), wait, screenshot to work/dev-shot.png
#   tools/device.sh shot [name] screenshot only -> work/<name>.png
#   tools/device.sh log [n]    last n TMRecomp logcat lines (default 80)
#   tools/device.sh sh '<cmd>' adb shell
#   tools/device.sh stop       force-stop the app
cd "$(dirname "$0")/.."
. tools/env.sh
SERIAL=$TM_SERIAL
PROJ=$TM_BUILDER_PROJ
PKG=ch.plum.tmturbo.recomp
mcp() { node /app/scripts/mcp-call.mjs call android-builder "$1" "$2" 2>&1; }
ash() { mcp adb_shell "$(python3 -c 'import json,sys; print(json.dumps({"serial":sys.argv[1],"command":sys.argv[2],"timeoutMs":int(sys.argv[3])}))' "$SERIAL" "$1" "${2:-60000}")" | python3 -c '
import sys,json
t=sys.stdin.read(); i=t.find("{")
try: print(json.loads(t[i:]).get("output",""), end="")
except Exception: print(t)'; }
case "$1" in
  apk)
    sh tools/sync_android.sh | tail -3
    mcp android_build "{\"projectId\":\"$PROJ\",\"variant\":\"debug\"}" | tail -15
    ;;
  install) mcp android_install "{\"projectId\":\"$PROJ\",\"serial\":\"$SERIAL\"}" | tail -8 ;;
  run)
    ash "am force-stop $PKG; logcat -c; input keyevent KEYCODE_WAKEUP; am start -n $PKG/.MainActivity --ez autostart true --ez unlock true ${TM_AM_EXTRA:-} | tail -1"
    ;;
  shot)
    N=${2:-dev-shot}
    mcp adb_screenshot "{\"serial\":\"$SERIAL\",\"outputPath\":\"/app/projects/$PROJ/dev-shot.png\"}" | tail -3
    cp "$TM_BUILDER_DIR/dev-shot.png" work/$N.png 2>/dev/null && ls -la work/$N.png
    ;;
  log) ash "logcat -d -s TMRecomp:V | tail -n ${2:-80}" ;;
  sh) ash "$2" "${3:-60000}" ;;
  stop) ash "am force-stop $PKG" ;;
  *) sed -n 2,9p "$0" ;;
esac
