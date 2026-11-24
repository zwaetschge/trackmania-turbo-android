#!/bin/sh
# Build libvpx (VP8 decoder only, NEON) for Android arm64 inside the build container:
#   DOCK_ARGS="-v <android-sdk>:/sdk:ro" TM_IMAGE_TAG=4 tools/dock.sh tools/build_libvpx.sh
# Source: third_party/libvpx (git clone --depth 1 -b v1.14.1 https://github.com/webmproject/libvpx.git).
# Result: work/libvpx-android/libvpx.a, headers in third_party/libvpx/vpx/.
# The runtime's video hook (runtime/src/video.c) decodes the game's VP8 video with it
# instead of running the game's own lifted x86 libvpx.
set -e
NDK=/sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin
OUT=/w/work/libvpx-android
mkdir -p "$OUT" && cd "$OUT"
CC="$NDK/aarch64-linux-android29-clang" CXX="$NDK/aarch64-linux-android29-clang++" AR="$NDK/llvm-ar" \
AS="$NDK/aarch64-linux-android29-clang" LD="$NDK/aarch64-linux-android29-clang" NM="$NDK/llvm-nm" STRIP="$NDK/llvm-strip" \
/w/third_party/libvpx/configure --target=arm64-linux-gcc \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-docs \
  --disable-vp9 --disable-vp8-encoder --enable-vp8-decoder --disable-webm-io --disable-libyuv \
  --enable-pic --disable-shared --enable-static --disable-runtime-cpu-detect --enable-multithread \
  --extra-cflags="-O3"
make -j"${JOBS:-8}" libvpx.a
ls -la "$OUT/libvpx.a"
