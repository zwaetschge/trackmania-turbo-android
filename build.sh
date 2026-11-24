#!/bin/sh
# Builds the Android APK from your own Trackmania Turbo (PC) installation, in Docker.
#   ./build.sh "/path/to/Trackmania Turbo"      (the game folder)
#   ./build.sh "/path/to/TrackmaniaTurbo.zip"   (or a ZIP of it)
# Result: out/TrackmaniaTurbo-Android-<version>.apk   (see README.md)
set -e
cd "$(dirname "$0")"
GAME=${1:?usage: ./build.sh <game folder or ZIP>}
[ -e "$GAME" ] || { echo "$GAME does not exist"; exit 2; }
GAME_ABS=$(cd "$(dirname "$GAME")" && pwd)/$(basename "$GAME")
# The Android SDK/NDK tools only exist for x86-64 Linux; on ARM hosts (Apple Silicon) Docker emulates it.
docker build --platform linux/amd64 -t tmturbo-build build
mkdir -p work/home
docker run --rm --platform linux/amd64 -u "$(id -u):$(id -g)" -e HOME=/w/work/home ${JOBS:+-e JOBS=$JOBS} \
  -v "$PWD:/w" -v "$GAME_ABS:/game:ro" tmturbo-build sh /w/tools/build_all.sh /game
