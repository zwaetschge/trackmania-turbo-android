#!/bin/sh
# Run a command in the host-dev container with the project mounted at /w.
cd "$(dirname "$0")/.."
. tools/env.sh
exec docker run --rm -u "$(id -u):$(id -g)" -v "$TM_HOST_ROOT:/w" -w /w ${DOCK_ARGS:-} tm-hostdev:${TM_IMAGE_TAG:-3} sh -c "$*"
