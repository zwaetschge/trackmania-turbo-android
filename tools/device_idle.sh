#!/bin/sh
# Seconds since the last "user activity" the power manager saw (includes focus changes), the
# seconds since the last real key/motion event (input_s), and the focused window. The device is
# shared with the player: launch/install/force-stop only after 600 s without input.
cd "$(dirname "$0")/.."
tools/device.sh sh 'P=$(dumpsys power); W=$(echo "$P" | grep -o "mLastWakeTime=[0-9]* ([0-9]* ms ago)"); A=$(echo "$P" | grep -o "mLastUserActivityTime(excludingAttention)=[0-9]*" | grep -o "[0-9]*$"); WT=$(echo "$W" | grep -o "=[0-9]*" | tr -d =); AGO=$(echo "$W" | grep -o "([0-9]*" | tr -d "("); E=$(dumpsys input 2>/dev/null | grep -A12 "RecentQueue" | grep -E "KeyEvent|MotionEvent" | sed "s/.*age=\([0-9]*\)ms.*/\1/" | sort -n | head -1); echo "idle_s=$(( (WT + AGO - A) / 1000 )) input_s=$(( ${E:-9999000} / 1000 )) $(echo "$P" | grep -o "mWakefulness=[A-Za-z]*") $(dumpsys window | grep -o "mCurrentFocus=.*" | sed "s/.* u0 //; s/}//")"; true' | tail -1
