#!/bin/sh
# Copy the Android project (including the locally built libtmturbo.so) into the
# android-builder workspace, which is owned by uid 1100.
cd "$(dirname "$0")/.."
. tools/env.sh
exec docker run --rm -u 1100:1100 \
  -v "$TM_HOST_ROOT/android:/src:ro" \
  -v "${TM_BUILDER_HOST_DIR:-$TM_BUILDER_DIR}:/dst" \
  ${TM_SYNC_IMAGE:-tmturbo-build} sh -c 'cp -r /src/. /dst/ && rm -rf /dst/native && ls -la /dst /dst/app/src/main/jniLibs/arm64-v8a'
