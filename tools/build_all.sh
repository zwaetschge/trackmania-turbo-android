#!/bin/sh
# Build the Android APK from your own Trackmania Turbo (PC) installation.
#   tools/build_all.sh <game folder | ZIP of it>
# Normally run through ./build.sh (Docker image build/Dockerfile, repo mounted at /w).
# Everything derived from the game -- ref/ (copies of the EXE and two DLLs), gen/ (the
# recompiled C code), work/, the .so and the APK -- stays on this machine (.gitignore).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
SRC=${1:?usage: tools/build_all.sh <game folder or ZIP>}
PY=${PY:-python3}
export PCRECOMP="$ROOT/third_party/pcrecomp"
MEM_GB=$(awk '/MemAvailable/ {print int($2/1048576)}' /proc/meminfo)
CPUS=$(nproc)
pick() { n=$((MEM_GB / $1)); [ "$n" -lt 1 ] && n=1; [ "$n" -gt "$CPUS" ] && n=$CPUS; [ "$n" -gt "$2" ] && n=$2; echo $n; }
LIFT_JOBS=${LIFT_JOBS:-$(pick 4 8)}      # a lift shard holds the whole function map (~3 GB)
export JOBS=${JOBS:-$(pick 4 64)}        # an -O1 compile of a big generated file needs up to ~3-4 GB
step() { echo; echo "=== $(date +%H:%M:%S) $*"; }
step "1/7 game files from $SRC ($CPUS CPUs, ${MEM_GB} GB free: $LIFT_JOBS lift / $JOBS compile jobs)"
mkdir -p ref work out
$PY - "$SRC" <<'PYEOF'
import hashlib, os, shutil, sys, zipfile
src = sys.argv[1]
need = {"trackmaniaturbo.exe": "TrackmaniaTurbo.exe", "audiomotors_shared_mt.dll": "audiomotors_shared_mt.dll", "libwebp.dll": "libwebp.dll"}
want = {}
for line in open("port/ref-hashes.sha256"):
    h, p = line.split(); want[os.path.basename(p)] = h
found = {}
if os.path.isdir(src):
    for root, _, files in os.walk(src):
        for f in files:
            if f.lower() in need and need[f.lower()] not in found and not os.path.relpath(root, src).count(os.sep) > 1:
                found[need[f.lower()]] = os.path.join(root, f)
    for name, p in found.items(): shutil.copyfile(p, os.path.join("ref", name))
elif zipfile.is_zipfile(src):
    with zipfile.ZipFile(src) as z:
        for m in z.infolist():
            b = os.path.basename(m.filename.replace("\\", "/")).lower()
            if b in need and need[b] not in found:
                found[need[b]] = m.filename
                with z.open(m) as i, open(os.path.join("ref", need[b]), "wb") as o: shutil.copyfileobj(i, o)
else:
    sys.exit(f"{src}: neither a folder nor a ZIP file")
missing = [n for n in need.values() if n not in found]
if missing: sys.exit("not found in the game files: " + ", ".join(missing))
for name in need.values():
    h = hashlib.sha256(open(os.path.join("ref", name), "rb").read()).hexdigest()
    if h != want[name]:
        sys.exit(f"{name}: SHA-256 {h} -- this port needs the PC version 2016-11-07 (expected {want[name]})")
    print(f"  {name}: ok")
PYEOF
step "2/7 third-party sources"
sh tools/fetch_deps.sh
step "3/7 function maps"
$PY tools/catalog_pack.py unpack port/catalog/TrackmaniaTurbo.json.xz work/catalog.json
$PY tools/catalog_pack.py unpack port/catalog/TrackmaniaTurbo-extra.json.xz work/catalog_extra.json
step "4/7 lifting x86 -> C (8 parts, $LIFT_JOBS at a time; the generated code is ~2.5 GB)"
mkdir -p gen/extra
seq 0 7 | xargs -P "$LIFT_JOBS" -I{} sh -c "$PY -u tools/run_lift.py lift ref/TrackmaniaTurbo.exe work/catalog.json gen --shard {}/8 > work/lift_s{}.log 2>&1 || { echo 'lift part {} failed: see work/lift_s{}.log'; exit 255; }"
$PY -u tools/run_lift.py lift ref/TrackmaniaTurbo.exe work/catalog_extra.json gen/extra --prefix extra_ > work/lift_extra.log 2>&1
$PY tools/lift_dll.py audiomotors_shared_mt.dll am 10000000 > work/lift_am.log 2>&1
$PY tools/lift_dll.py libwebp.dll wp 10100000 > work/lift_wp.log 2>&1
echo "  $(ls gen/*.c gen/extra/*.c gen/am/*.c gen/wp/*.c | wc -l) C files"
step "5/7 DXVK (Direct3D 11 -> Vulkan) for Android"
$PY gfx/dxvk/prepare.py third_party/dxvk-native work/dxvk-source
$PY gfx/dxvk/shaders.py work/dxvk-source
step "6/7 compiling libtmturbo.so for arm64 (the long part: ~20 min on 16 cores, hours on few)"
sh tools/build_android_so.sh "$ROOT/gen" > work/build-android.log 2>&1 || { tail -30 work/build-android.log; echo "native build failed: see work/build-android.log"; exit 1; }
tail -1 work/build-android.log
step "7/7 APK"
echo "sdk.dir=${ANDROID_SDK:-/sdk}" > android/local.properties
export GRADLE_USER_HOME="$ROOT/work/gradle" ANDROID_USER_HOME="$ROOT/work/android-user"   # the signing key stays the same between builds
(cd android && sh ./gradlew --no-daemon -q assembleDebug)
VER=$(sed -n 's/.*versionName = "\(.*\)".*/\1/p' android/app/build.gradle.kts)
cp android/app/build/outputs/apk/debug/app-debug.apk "out/TrackmaniaTurbo-Android-$VER.apk"
step "done: out/TrackmaniaTurbo-Android-$VER.apk"
ls -la "out/TrackmaniaTurbo-Android-$VER.apk"
