#!/bin/sh
# Build libtmturbo.so for arm64-v8a with the local NDK and drop it into jniLibs.
#   tools/build_android_so.sh [GEN_DIR|stub] [ninja-target...]
# env: GUEST_OPT (default -O1), JOBS (default 5: -O1 on the big lifted files
# needs several GB per compiler process).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK:-/sdk}
NDK=$SDK/ndk/27.3.13750724
CMAKE=$SDK/cmake/3.31.6/bin/cmake
GEN=${1:-$ROOT/gen}; [ $# -gt 0 ] && shift
if [ "$GEN" = stub ]; then OPTS="-DSTUB=ON"; BD=$ROOT/work/build-android-stub; else OPTS="-DGEN_DIR=$GEN"; BD=$ROOT/work/build-android; fi
# The native VP8 decoder (runtime/src/video.c) is linked when its library is there; build it
# once if the source is (git clone --depth 1 -b v1.14.1 https://github.com/webmproject/libvpx.git third_party/libvpx).
if [ ! -f $ROOT/work/libvpx-android/libvpx.a ] && [ -f $ROOT/third_party/libvpx/configure ]; then
  sh $ROOT/tools/build_libvpx.sh > $ROOT/work/build-libvpx.log 2>&1 || echo "libvpx did not build (see work/build-libvpx.log): the game's own slow decoder stays in use"
fi
$CMAKE -S $ROOT/android/native -B $BD -G Ninja -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.31.6/bin/ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DGUEST_OPT=${GUEST_OPT:--O1} "-DGUEST_HOT_OPT=${GUEST_HOT_OPT:-}" $OPTS > $BD.cmake.log 2>&1 || { tail -30 $BD.cmake.log; exit 1; }
if [ $# -gt 0 ]; then $SDK/cmake/3.31.6/bin/ninja -C $BD -j${JOBS:-5} "$@"; exit $?; fi
$SDK/cmake/3.31.6/bin/ninja -C $BD -j${JOBS:-5}
mkdir -p $ROOT/android/app/src/main/jniLibs/arm64-v8a
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-debug -o $ROOT/android/app/src/main/jniLibs/arm64-v8a/libtmturbo.so $BD/libtmturbo.so
ls -la $BD/libtmturbo.so $ROOT/android/app/src/main/jniLibs/arm64-v8a/
