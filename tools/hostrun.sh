#!/bin/sh
cd "$(dirname "$0")/.."
. tools/env.sh
GAME=${TM_GAME_DIR:?set TM_GAME_DIR (the game folder) or put it in work/local.env}
docker run --rm -u "$(id -u):$(id -g)" \
  -v "$TM_HOST_ROOT:/w" -w /w \
  -v "$GAME:/game:ro" ${DOCK_ARGS:-} tm-hostdev:${TM_IMAGE_TAG:-4} sh -c \
  "ninja -C work/build-host -j10 2>&1 | grep -E 'error|FAILED' | head -40; \
   [ -x work/build-host/tmhost ] || exit 9; \
   export DISPLAY=:99; Xvfb :99 -screen 0 1280x720x24 >/tmp/xvfb.log 2>&1 & sleep 2; \
   TM_GPU_VENDOR='${TM_GPU_VENDOR:-}' TM_GPU_DEVICE='${TM_GPU_DEVICE:-}' TM_ICALL_COLLECT='${TM_ICALL_COLLECT:-}' TM_TRACE='${TM_TRACE:-}' timeout ${TIMEOUT:-120} work/build-host/tmhost /game/TrackmaniaTurbo.exe /w/work/user > work/hostrun.log 2>&1; echo exit \$?; \
   import -window root /w/work/host-screen.png 2>/dev/null || xwd -root -silent 2>/dev/null | convert xwd:- /w/work/host-screen.png 2>/dev/null || true"
