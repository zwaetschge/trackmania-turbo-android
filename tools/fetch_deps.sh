#!/bin/sh
# Third-party sources at the exact revisions this port was built and tested with.
# usage: tools/fetch_deps.sh            (skips what is already there)
set -e
cd "$(dirname "$0")/.."
mkdir -p third_party
clone_tag() {   # dir url tag
  [ -d "third_party/$1/.git" ] && return 0
  echo "fetch $1 $3"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "third_party/$1"
}
clone_tag dxvk-native      https://github.com/Joshua-Ashton/dxvk-native.git native-1.9.2b   # c8dc91fa, zlib
clone_tag openal-soft      https://github.com/kcat/openal-soft.git          1.23.1          # LGPL-2.1
clone_tag libvpx           https://github.com/webmproject/libvpx.git        v1.14.1         # BSD
clone_tag nfsu2-recompiled https://github.com/codepdbh/nfsu2-recompiled     v0.14.13        # MIT (two Vulkan headers)
# The lifter: upstream pcrecomp (MIT) at 35548b8 plus this port's changes (tools/patches/pcrecomp).
if [ ! -d third_party/pcrecomp/.git ]; then
  echo "fetch pcrecomp 35548b8 + $(ls tools/patches/pcrecomp/*.patch | wc -l) patches"
  git clone -q https://github.com/sp00nznet/pcrecomp third_party/pcrecomp
  git -C third_party/pcrecomp -c advice.detachedHead=false checkout -q 35548b8d0fd8aa853da67edcf7cd6134f6b9cdc7
  git -C third_party/pcrecomp -c user.name=build -c user.email=build@localhost am -q "$PWD"/tools/patches/pcrecomp/*.patch
  test "$(git -C third_party/pcrecomp rev-parse HEAD^{tree})" = 45a5b6ca81654228c8da6e97eef58a40224e7ee6 \
    || { echo "pcrecomp: patched tree differs from the tested one"; exit 1; }
fi
echo "third-party sources ready"
