# Sourced by device scripts that inject input: never press buttons into a session the
# player has taken over.  guard_init once, then "guard_press <adb input args...>" for every
# injected event; it stops the script (exit 3) when the device saw input that was not ours.
#   guard_start <minimum idle seconds, default 600>   refuses to start on a device in use
D=${D:-tools/device.sh}
_guard_last=0
_guard_grace=4        # seconds after our own last event in which input still counts as ours (a launch
                      # makes the system register activity for a while: set 25 before the first check)
# Seconds since the last real input event (key or stick/touch motion, ours included). The
# power manager's "last user activity" is no use for this: it also moves when a window gains
# focus, which our own launches, installs and the game's track loading cause.
# The queue holds only the last 10 events; when other events (e.g. the controller's periodic
# DeviceResetEvent) have pushed every key/motion event out, fall back to the power manager.
_guard_idle() { $D sh 'A=$(dumpsys input 2>/dev/null | grep -A12 "RecentQueue" | grep -E "KeyEvent|MotionEvent" | sed "s/.*age=\([0-9]*\)ms.*/\1/" | sort -n | head -1); if [ -n "$A" ]; then echo $(( A / 1000 )); else P=$(dumpsys power); W=$(echo "$P" | grep -o "mLastWakeTime=[0-9]* ([0-9]* ms ago)"); L=$(echo "$P" | grep -o "mLastUserActivityTime(excludingAttention)=[0-9]*" | grep -o "[0-9]*$"); WT=$(echo "$W" | grep -o "=[0-9]*" | tr -d =); AGO=$(echo "$W" | grep -o "([0-9]*" | tr -d "("); echo $(( (WT + AGO - L) / 1000 )); fi; true' 20000 | tail -1; }
guard_start() {
  I=$(_guard_idle); MIN=${1:-600}
  case "$I" in ''|*[!0-9]*) echo "guard: cannot read the idle time ($I)"; exit 3;; esac
  if [ "$I" -lt "$MIN" ]; then echo "guard: device in use (last input $I s ago, need $MIN) - not starting"; exit 3; fi
  _guard_last=$(date +%s)
}
guard_check() {                      # input since our own last event?
  I=$(_guard_idle); NOW=$(date +%s); MINE=$((NOW - _guard_last))
  case "$I" in ''|*[!0-9]*) return 0;; esac
  if [ "$I" -lt $((MINE - ${_guard_grace:-4})) ]; then echo "guard: the player is using the device (input $I s ago, ours $MINE s ago) - stopping"; exit 3; fi
  _guard_grace=4
}
# Press only while the game is the focused window: a crashed game must not turn our presses
# into launcher input (2026-11-11: an A press started another app from the launcher).
guard_press() {
  guard_check
  # (the focus is briefly elsewhere -- or null -- while the game switches surfaces: retry)
  R=$($D sh "for i in 1 2 3 4 5 6; do if dumpsys window 2>/dev/null | grep -q 'mCurrentFocus=.*ch.plum.tmturbo.recomp'; then input $* && echo pressed; break; fi; sleep 1; done; true" 30000 | grep -c pressed)
  _guard_last=$(date +%s); echo $_guard_last > work/.guard_last
  [ "${R:-0}" -ge 1 ] || { echo "guard: the game is not the focused window (crashed or left) - stopping"; exit 3; }
}
# Continue a session an earlier script of ours left running: fine when the device saw no
# input since our last event (kept in work/.guard_last), or has been idle long enough.
guard_resume() {
  I=$(_guard_idle); NOW=$(date +%s); L=$(cat work/.guard_last 2>/dev/null || echo 0)
  case "$I" in ''|*[!0-9]*) echo "guard: cannot read the idle time ($I)"; exit 3;; esac
  if [ "$I" -lt $((NOW - L - 6)) ] && [ "$I" -lt ${1:-600} ]; then echo "guard: the player used the device $I s ago (our last input was $((NOW - L)) s ago) - not continuing"; exit 3; fi
  _guard_last=$L
}
guard_mark() { _guard_last=$(date +%s); echo $_guard_last > work/.guard_last; }
