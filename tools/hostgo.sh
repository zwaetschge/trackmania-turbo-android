#!/bin/sh
# Build the host binary and run it under Xvfb.
# usage: hostgo.sh <logname> [timeout_s] [shot_every_s]
# Writes work/<logname>.log, work/<logname>.png (last screenshot) and, with
# shot_every_s, work/shots/<logname>-NNN.png every N seconds.
cd "$(dirname "$0")/.."
. tools/env.sh
LOG=${1:-hostrun}; T=${2:-300}; EVERY=${3:-0}
GAME=${TM_GAME_DIR:?set TM_GAME_DIR (the game folder) or put it in work/local.env}
mkdir -p work/shots
docker run --rm -u "$(id -u):$(id -g)" -v "$TM_HOST_ROOT:/w" -w /w -v "$GAME:/game:ro" tm-hostdev:${TM_IMAGE_TAG:-4} sh -c "
  ninja -C work/build-host -j6 2>&1 | grep -E 'error|FAILED|ninja: build stopped' | head -20;
  export DISPLAY=:99; Xvfb :99 -screen 0 1280x720x24 >/tmp/xvfb.log 2>&1 & sleep 2;
  export TM_GPU_VENDOR=0x1002 TM_GPU_DEVICE=0x6718 ${TM_ENV:-};
  if [ $EVERY -gt 0 ]; then (i=0; while sleep $EVERY; do i=\$((i+1)); import -window root /w/work/shots/$LOG-\$(printf %03d \$i).png 2>/dev/null; cp /w/work/shots/$LOG-\$(printf %03d \$i).png /w/work/$LOG.png 2>/dev/null; done) &
  else (sleep $((T-8)); import -window root /w/work/$LOG.png 2>/dev/null) & fi
  timeout $T work/build-host/tmhost /game/TrackmaniaTurbo.exe /w/work/user > work/$LOG.log 2>&1; echo \"exit \$?\";
  [ -f /w/work/$LOG.png ] || import -window root /w/work/$LOG.png 2>/dev/null || true"
echo "icalls: $(grep -oE 'icalls [0-9]+' work/$LOG.log | tail -1)  lines=$(wc -l < work/$LOG.log)"
grep -E "FATAL|guest state|unimplemented" work/$LOG.log | tail -4 | cut -c1-300
