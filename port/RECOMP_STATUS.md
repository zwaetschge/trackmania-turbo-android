# Trackmania Turbo → Android (arm64) — static recompilation status

Evidence ledger. "Verified" means executed and observed; anything else is marked.

## Gate 0 — target (verified by static inspection, 2026-11-09)

| Item | Evidence |
|---|---|
| Source install | the owner's PC install (Uplay build, 3.9 GB), originals untouched; read-only working copies in `ref/` (gitignored), hashes in `port/ref-hashes.sha256` |
| Main module | `TrackmaniaTurbo.exe` PE32 **x86**, 26,473,048 bytes, SHA-256 `454b0d4d9eb778a298e523c20f9bb0b9e5459217afd45566dfcffe9bb8312879`, `Distro=UPLAYe201611041519_d201611041250` (nadeo.ini) |
| Layout | image base 0x00400000, entry 0x00EEE7D0, `.text` 19.5 MB (0x401000–0x16A2FF7), base relocations present (552,891 HIGHLOW), static TLS (0x1025 bytes, no callbacks), 10,456 SafeSEH handlers, no delay imports, no exports |
| Packing/DRM | not packed (`.text` entropy 6.43, normal sections). Licensing via `uplay_r1_loader.dll` (24 imports incl. `UPLAY_USER_IsOwned`), Steamworks imports also present (same EXE serves both distros) |
| CRT | static MSVC 2015 CRT/UCRT (no `VCRUNTIME140`/`api-ms-win-crt` imports) |
| Imports | 475 across 28 DLLs (`port/imports.json`): kernel32 212, user32 60, ws2_32 33, d3d11 `D3D11CreateDevice`, dxgi `CreateDXGIFactory1`, dinput8, xinput9_1_0, d3dcompiler_46 (`D3DReflect`, `D3DGetInputSignatureBlob`), wininet, avifil32, audiomotors, vorbis (encoder only), libwebp, openvr |
| Runtime-loaded DLLs | `OpenAl32(_bundled).dll` (audio via LoadLibrary/GetProcAddress), `BINKW32.DLL`, `nvapi.dll`, `psapi.dll`, `d3d9.dll`, `7DDRAW.DLL` strings |
| Instruction mix (linear sweep, approx.) | 5.89 M instructions; SSE1 267k, SSE2 148k, x87 8.7k, MMX 4.8k, CMOV 7.7k, AVX/AVX2 ~7.5k, SSSE3/SSE4 ~450 (CPU-dispatched codec paths) — `port/insn-census-linear.json` |
| Graphics | Direct3D 11 + DXGI (SM4/SM5 shader caches in `GameData/GpuCache_D3D11_SM*.zip`) |
| RTTI | 433 classes only (engine uses its own class system); 8,422 anonymous vtables found structurally |

### Top risks
1. **D3D11/DXGI → Vulkan on Android**: needs a guest-COM bridge over DXVK-native (d3d11+dxgi) with 32-bit guest structs, Map/Unmap staging, swap chain on `ANativeWindow`. Largest single work item.
2. **Platform licensing (Uplay)**: on Android `UPLAY_Start` honestly fails. The engine's ownership helper (0x007757A0) only calls `UPLAY_USER_IsOwned` when the Uplay object exists; the behaviour after a failed start is not yet observed. The port will not fake ownership; if the game refuses to continue, that is a blocker for the user to decide.
3. **Code size**: ~190k functions / ~5.9 M instructions → very large generated C (compile time, .so size, ThinLTO memory).
4. **Threads + memory model**: multi-threaded engine; first version serialises guest threads with a machine lock (correct on ARM64, single-core guest speed).
5. **SEH / C++ exceptions**: `RaiseException`/`RtlUnwind` are not implemented yet; they stop with a diagnostic if reached.

## Gate 1 — route (decided)

- **AOT PE32**: pcrecomp `disasm32` + `lift32` at commit 35548b8 + the 11 NFSU2 patches (`third_party/nfsu2-recompiled/patches/pcrecomp`) + `tools/patches/pcrecomp/0001` (RECOMP_TLS: per-guest-thread register file).
- Function seeds: entry, direct call/jmp, prologues, RTTI (1,370), vtable scan (34,261), **base-relocation targets referenced from data** (91,310) → 104,719 extra seeds (`tools/reloc_seeds.py`).
- Own portable runtime (`runtime/`, C11): 4 GB guest space at a host offset (guest VAs preserved), FIFO machine lock, TLS register file, PE loader/IAT binder with synthetic import VAs, strict stops on unimplemented imports/instructions/indirect targets.
- Desktop proof host: Linux x86-64 (`host/`, built in the `tm-hostdev` container). Android: NDK r27 `libtmturbo.so` + Java boot shell (`android/`).
- Virtual CPU: CPUID reports SSE2-class CPU (no SSE3/SSSE3/SSE4/AVX) so dispatched codec paths take SSE2 fallbacks; RDTSC = monotonic 1 GHz.
- Fallback if lifting quality blocks progress: per-function bisection against original code is not available on ARM; desktop host + difftests are the debugging path.

## Milestones

| Milestone | State |
|---|---|
| M0 target inventoried | **verified** |
| M1 function-lift difftests pass | **verified on host** (sampled, Unicorn oracle, Android compiler flags): random leaf 590/606 cases, SSE leaf 252/266, x87 leaf 331/350, new lifter instructions 65/65. Explained mismatches: mid-function alias entries returning into random stacks, `movaps/movdqa` alignment faults the lift does not emulate, CPUID-gated SSE4.1, NaN payload choice in one SSE loop (0x00481B40). `fnstsw` without a preceding compare reported C3=1 (per-function `_fpu_cmp` started at "equal", CRT `_statusfp` 0x00F08618): **fixed in lift32** (patch 0003, 8/8 cases equal); takes effect in builds regenerated after 2026-11-09 20:45. `tools/difftest_real.py` |
| M2 initialization / desktop trace | **verified on Linux x86-64 host**: static CRT init (~40k constructors), WinMain, build-info (VERSIONINFO), nadeo.ini, user dirs, 2 engine threads, DXGI factory/adapter enumeration, D3D11 feature-level probe; stops at the Uplay gate (see below) |
| M3 Android boots game code | **verified on device**: Odin3 (Snapdragon 8 Elite, Android 15), APK 0.1.0-boot with `libtmturbo.so` 698 MB (-O1, all 193,792 functions). Same path as the host up to the Uplay gate and `ExitProcess(0)`; evidence `port/evidence/odin3-boot.png`, `odin3-boot-logcat.txt`, host log `host-boot.log` |
| M4 interactive menu | **verified on device** (2026-11-10): intro video, title, offline notice, main menu, campaign, track grid, all with the built-in gamepad; evidence `work/dev-y-track.png` |
| M5 gameplay | **verified on device** (2026-11-10): campaign track #01 loads in ~30 s, pre-race panel, countdown, the car accelerates, steers, drifts, crashes, restarts; HUD, timer, music; evidence `work/dev-v2.png`, `work/dev-drive1.png` |
| M6 performance/stability | **in progress**: ~95 fps in-race at 1920x1080 on the Odin3 (was 19 fps before the 2026-11-10 performance work, see the last section); soak runs of a few minutes only; not yet: finishing a race, saving progress across launches, app pause/resume |

## Offline launch (host, 2026-11-09)

The owner runs their own copy offline without a Ubisoft Connect account.
`runtime/src/uplay.c` reproduces Uplay's own offline mode: `UPLAY_Start` returns
success (0), `UPLAY_USER_IsOwned` returns owned (1), `IsInOfflineMode` true,
`IsConnected` false; friends/overlay/achievements are offline no-ops. Return
conventions recovered from this build (0x00775510 init wrapper, 0x007757A0
ownership check). Nothing contacts a server.

With that, the lifted game runs ~253k indirect calls of real engine code past
the gate: nadeo.ini config, Config/*.SystemConfig.Gbx written, language en_US,
`Packs/ManiaPlanet.pak` opened, then it reflects its precompiled shaders and
stops at `d3dcompiler_46.dll!D3DReflect` (next work item).

## Current boundary (host): real Direct3D 11 device creation

With offline Uplay, shader reflection (`runtime/src/d3dcompiler.c`: D3DReflect +
D3DGetInputSignatureBlob over the game's DXBC), DirectInput8 + XInput
(`runtime/src/dinput8.c`), and DXGI adapter/output enumeration all working, the
lifted engine now initialises fully and calls `D3D11CreateDevice` with a real
device out-param (flags 0x21, 2 feature levels). It reflects its vertex and
pixel shaders correctly (cbuffers, bindings, in/out signatures) along the way.

**Next major work item: the Direct3D 11 → Vulkan device bridge.** DXVK-native
(d3d11+dxgi, prepared in `gfx/dxvk`, built headless on lavapipe) produces native
C++ `ID3D11Device` objects; the guest calls their methods through 32-bit guest
vtables. Needed: a guest-COM ↔ native-DXVK marshalling layer — `ID3D11Device`
(~43 methods), `ID3D11DeviceContext` (~115), resources/views/shaders/states/
queries and `IDXGISwapChain`, each translating 32-bit guest structs to native.
`runtime/src/com.c` + the renderer self-test (`gfx/test/gfx_selftest.cpp`, which
already creates a DXVK device and translates every game shader) are the
foundation. This is a multi-session effort — the bulk of a D3D11 port.

## Lift numbers
- catalog: 193,792 functions (17 discovery rounds + data-pointer scan + branch-target closure), ~72k of them MSVC EH funclets (0x0152805B-0x015C4870)
- generated C: 48.9 M lines in 488 files, 0 lift failures, 132,059 ICALL and 97,263 ITAIL sites
- remaining RECOMP_UNIMPL sites: SSSE3/SSE4.1/AVX/AVX2 (CPUID reports an SSE2 CPU) and bytes that only decode as BCD/privileged opcodes

## Runtime coverage
- Loader verified on desktop host: image mapped at 0x00400000, 475 imports bound, 356 with implementations (kernel32 core/file/string/locale, user32 window+message core, advapi32, shell32, ole32, rpcrt4, version, iphlpapi, setupapi, Uplay/Steam/OpenVR "unavailable").
- Missing (stop when called): d3d11, dxgi, dinput8, xinput, d3dcompiler, ws2_32, wininet, avifil32, audiomotors, vorbis, libwebp, a few kernel32/user32 entries.

## Reproduce
```sh
python3 -m venv .venv && .venv/bin/pip install pefile capstone unicorn numpy
.venv/bin/python -I tools/reloc_seeds.py ref/TrackmaniaTurbo.exe work/reloc_seeds.json
# rtti.py / vtable_scan.py from third_party/pcrecomp/tools/cpp -> work/*_seeds.json, merged into work/seeds_all.json
PCRECOMP=third_party/pcrecomp .venv/bin/python tools/run_lift.py catalog ref/TrackmaniaTurbo.exe work/seeds_all.json work/catalog.json
PCRECOMP=third_party/pcrecomp .venv/bin/python tools/run_lift.py lift ref/TrackmaniaTurbo.exe work/catalog.json gen
tools/dock.sh 'cmake -S host -B work/build-host -G Ninja -DGEN_DIR=/w/gen -DCMAKE_C_COMPILER=clang && ninja -C work/build-host'
DOCK_ARGS="-v <android-sdk with NDK 27.3>:/sdk:ro" tools/dock.sh 'tools/build_android_so.sh /w/gen'
```

## Next work items (in order)

1. **Licensing gate (user decision).** The game exits when `UPLAY_Start` fails.
   Proceeding needs either an explicit decision by the owner to run the owned
   copy with an offline platform bridge, or another authorized way to satisfy
   the platform layer. Not done autonomously.
2. **Direct3D 11 backend** — DXVK-native `native-1.9.2b` (pin c8dc91fa, cloned in
   `third_party/dxvk-native`) built for arm64 with `d3d11`, `dxgi`, `dxbc`,
   `dxvk`, `spirv`, `vulkan`, `util` + the NFSU2 Android WSI (`ANativeWindow`).
   Built-in shaders need `glslangValidator`/`spirv-val` in `tm-hostdev`.
3. **Guest COM bridge for D3D11** (`runtime/src/com.c` exists): ID3D11Device (43
   methods), ID3D11DeviceContext (115), resources/views/shaders/states/queries,
   IDXGISwapChain. 32-bit guest structs → native structs (pointer fields:
   `D3D11_SUBRESOURCE_DATA`, `D3D11_INPUT_ELEMENT_DESC.SemanticName`, etc.);
   `Map` returns a guest staging window copied on `Unmap`; swap chain on the
   activity surface.
4. **d3dcompiler_46** (`D3DReflect`, `D3DGetInputSignatureBlob`): host DXBC
   chunk parser (ISGN/OSGN/RDEF) for the precompiled `GpuCache_D3D11_SM*.zip`.
5. Input (XInput from Android gamepad; DirectInput8 COM), audio (OpenAL Soft
   native behind the runtime `LoadLibrary("OpenAl32.dll")` bridge,
   `audiomotors_shared_mt.dll` lift or reimplementation), sockets (ws2_32).
6. SEH/C++ exception dispatch (`RaiseException`/`RtlUnwind`) for the engine's
   top-level `try/catch` in WinMain (0x00856430).

## 2026-11-10 findings (host offline run far; two distinct correctness bugs)

With offline boot, shader reflection, DirectInput8, no-network stubs, the GPU-benchmark
skip (spoof a GPU in GfxDevicePerfs.txt), and 7,499 recovered indirect-only functions,
the lifted game runs **~3.9M indirect calls** on the host: creates a real DXVK D3D11
device + swap chain, reflects 296 shaders, issues draws. The **renderer self-test**
independently translates **all 8,705 game shaders (0 failures)**, compiles pipelines and
presents frames (gfx/test, work/gfx-frame.png) — the graphics stack is proven.

Two correctness bugs block the first game frame:
1. **Host (both -O0 and -O1), 0x00474ED0 at ~3.9M icalls:** a thiscall writes past a
   6-element array (crash at element 8) — an object reached via
   0x00D32FD0→0x00BFE770→0x00C61410→0x00440FC0→0x0043B380 has a corrupted count/array
   field. Same at -O0 and -O1, so it is a genuine data/lift bug, not optimization. The
   lifted loop itself is correct (verified). Crash-dump of the object shows its heap
   header is **sz=0x18** (24 bytes, class 1): the game allocated a 24-byte block but
   treats it as a ~0x60-byte object (reads [this+0x2c]=loop count, [this+0x50]=array),
   so it reads/writes past its allocation into the next block's header (which is why
   [this+0x28] = the heap magic 0x48504C56 and [this+0x2c] = garbage 0x02000000). Not a
   zeroing or free-list-reuse issue (both tested). Root: a wrong object pointer or a
   mis-computed allocation size reaching 0x00474ED0 from the construction chain above —
   the next thing to trace (which alloc produced the 0x18-byte block, and whether the
   size or the pointer is wrong).
2. **Device (arm64) 0x0055E660 at ~126k icalls (thiscall this==null):** does NOT
   reproduce on the x86-64 -O1 host (which runs to 3.9M). So it is arm64-architecture-
   specific, not an optimization/-O1 issue — a clang-arm64 miscompile or UB in the
   generated C, or an arm64 runtime issue. Needs on-device instrumentation to localize.
   (-O0 arm64 does not link: unwind tables exceed the 2GB relocation range.)

Reaching a playable menu is a multi-session effort: a tail of individual correctness
fixes like these, on a large engine (ManiaPlanet). Each fix advances the run (126k→3.9M
so far). The graphics/runtime/offline foundation is complete and verified.

---

## 2026-11-10 (later) — FIRST REAL FRAMES on the host: logos + intro render

**Verified (host x86-64, -O0, lavapipe/Xvfb):** the lifted game boots offline, runs its
main loop and renders the Ubisoft logo, the Nadeo logo and the Trackmania Turbo intro
through recompiled code → guest-COM D3D11 bridge → DXVK → Vulkan. No crash in a 590 s
run. Evidence: `work/run8-sheet.png` (19 screenshots, 30 s apart), `work/hostrun7.png`.

All four blockers on the way were defects of the port, not of the engine:

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | 0x00474ED0 SIGSEGV (resize on a data buffer), second `sub_0043B380` call from one linear frame | the **extra catalog had overlapping (unsplit) basic blocks** (hand tracer `tools/add_funcs.py`); the lifter emits each block's whole range and falls through textually, so a not-taken `jcc` at the end of an overlapped block re-executed the code after it — in any of the 7,499 extra functions | catalog rebuilt with pcrecomp's real disassembler (`tools/add_funcs2.py`), extras re-lifted (5.4 M → 4.1 M C lines, ITAIL sites 4,127 → 333); `tools/chk_overlap.py` reports 0 unsplit blocks in both catalogs; 3 garbage seeds excluded (`config/exclude.txt`) |
| 2 | "ITAIL to 00000010 from 004EEBFE" (vcall thunk on an object whose vtable slot held a string pointer) | `uplay.c` name/account/ticket getters wrote a string pointer through `ARG(0)`, but those functions take **no arguments** (engine wrappers 0x00775690/0x007756F0 tail-jump into them) — the write hit a live object's vtable | out-param writes removed (found with a gdb hardware watchpoint on the vtable slot) |
| 3 | SIGSEGV in 0x004F7100 after `IDirectInputDevice8::GetDeviceData` | shim popped 4 stack slots, the method has 5 (with `this`) → guest stack off by 4 | whole DirectInput method table re-checked against the real signatures |
| 4 | "ITAIL to 00EEFBB0 from 00EEF700" | MSVC `memmove`/`memcpy` use a jump table with a **negative index** (`neg ecx / jmp [ecx*4+UnwindDownVec+28]`); the disassembler only walked tables forwards | `disasm32.jump_table_targets` also walks backwards inside the function (patch 0004); the two functions re-disassembled in place (`tools/redo_funcs.py`), one generated file changed |

The temporary `0x0043B380` guard from the entry below is gone (it never fires with a
correct lift). Debug method that found these quickly: gdb in the container
(`set startup-with-shell off`), guest registers are the TLS globals `g_eax…g_esp`,
`watch *(unsigned*)(g_mem_base+VA)` on the corrupted guest address.

**Input (verified on host):** `runtime/src/input.c` holds a platform-neutral gamepad +
keyboard state; XInput controller 0, the DirectInput keyboard (immediate + buffered) and
WM_KEY*/WM_CHAR read from it. A scripted run (`TM_INPUT_SCRIPT=sec:pad:A,...`) skips the
logos and reaches the epilepsy warning, the TRACKMANIA title screen (3D blimp), the TURBO
billboard scene and the menu ("RECORD CENTER") — `work/run10-sheet.png`.

**Audio (built, not yet heard):** the game loads `OpenAl32.dll` with LoadLibrary and
resolves 132 AL/ALC/EFX/SOFT entry points by name. `audio/openal_bridge.c` (generated by
`tools/gen_openal_bridge.py`) forwards all 132 to a natively linked OpenAL Soft 1.23.1
(`third_party/openal-soft`, OpenSL ES on Android).

**Device (in progress):** game files imported to the Odin3 (56 files, 4,137,190,604 bytes,
matches `work/manifest.txt`). `android/native/CMakeLists.txt` now builds DXVK + the D3D11
bridge + the extra functions + OpenAL into `libtmturbo.so`; the activity renders into a
full-screen SurfaceView, maps the built-in gamepad to XInput and a keyboard to Windows
keys. Not yet run on the device with graphics.

---

## 2026-11-10 — host blocker #1 resolved; new blocker; it's a virtual-dispatch pattern

Debugged entirely under gdb in the tm-hostdev container (workflow saved to memory).
Corrected the earlier read of blocker #1: the object is **not** undersized. The healthy
singleton at [0x1bde594]=0xee6f860 (hdr sz 0x74, vtable 0x16b8640) is fully built by the
FIRST sub_0043B380(singleton) call. The crash is a **SECOND** sub_0043B380 call, same
guest esp, from the single linear sub_00440FC0 frame (no loop — still unexplained how it
reaches the one call site twice), this time with this=0xee6f9b0 = the singleton's +0x68
element **data buffer** (6×u32, vtable word == 0). sub_00474ED0 treats it as a container
and writes through this->0x20 (=0x1c) → SIGSEGV at guest 0x1c.

**Fix (runtime/src/overrides.c, new; wired in shims_all.c):** tm_add_override(0x0043B380)
installs guard_sub_0043B380 — if ARG(0) (this, from [esp+4]) has [this]==0 (not a
constructed polymorphic object) it drops the spurious resize (RETC), else delegates to the
real lifted sub_0043B380. Run advances past 0x474ED0 (3.886M → 3.892M icalls, into
Manialinks/profile init).

**New host blocker, 0x004EEBFE (`mov eax,[ecx]; jmp [eax+0xc0]`, a 2-level virtual
thunk):** reached via icall from visitor sub_005077E0 with this = a profile object
0x5B723F0 (sz 0x968) whose [+0] points to the string "Player" (the offline uplay name) —
i.e. [+0] is a std::string name ptr, not a vtable, so jmp [name+0xc0] = garbage 0x10
("ITAIL to unresolved VA 00000010"). Unlike the resize, a virtual *call* can't be dropped
(the caller needs a return). Both blockers are the **same class**: a collection/visitor
dispatches a virtual method on an object whose layout isn't the expected polymorphic one.
Open question: whether the 0x0043B380 guard fixes or merely masks (the mis-built collection
may be the shared root). Next: trace sub_005077E0's iteration to see why a profile struct
sits where a vtable'd object is expected.

---

## 2026-11-10 (morning) — playable on the device; performance 19 → ~95 fps

**State.** On the Odin3 the game boots offline, plays the intro, goes through title,
main menu, campaign and track grid, loads campaign track #01 in about 30 s and races:
countdown, acceleration (right trigger), steering (d-pad / stick), drift scoring, crash,
restart (B). 1920x1080, music and engine sound are produced (an AudioTrack of the app is
active, 48 kHz stereo, no underruns). Host (Linux x86-64, lavapipe) reaches the same
scene, about twenty times slower.

**The "INSERT COIN" pre-race screen is the game, not a bug.** Two hours went into this,
so for the record: the campaign mode script (readable in a memory dump, see tools below)
enters an attract mode when every `Input.Pads[*].IdleDuration` is at least
`C_IdleTimeMax` — about five seconds — and shows "INSERT COIN / OR PRESS (A)". While idle
the client layer keeps re-arming `TimerInput = Now + 200`, so the first press only ends
the idle state, the second brings the medal panel back and the third selects "DRIVE
ALONE". Scripted tests therefore press A three to four times, more than a second apart
(`tools/device_race.sh`).

**Root causes fixed since the previous section (all in our own code):**

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 5 | menus almost black | D3D11 `Map(WRITE_NO_OVERWRITE)` got an empty staging buffer and `Unmap` copied all of it back, wiping earlier vertices | persistent staging per buffer subresource (`gfx/d3d11_bridge.cpp`, "Map staging") |
| 6 | COM slot table would fill up | bridge `Release` decremented twice, `wrap()` leaked a native reference per cache hit | fixed; unimplemented COM methods are named stubs that warn once |
| 7 | crash in 0x00EB4000 when a track starts | the engine plays solo through its own client/server on 127.0.0.1; all Winsock calls failed | `runtime/src/net.c`: real sockets, loopback only by policy; WININET stays "cannot connect" |
| 8 | "unimplemented import audiomotors_get_size" | bundled DLLs were not part of the lift | `tools/lift_dll.py`; `audiomotors_shared_mt.dll` and `libwebp.dll` are lifted as extra modules and mapped at fixed bases (`loader.c`) |
| 9 | in-race picture a flat tan colour on the device | Adreno 830 driver 512.800.72: `gl_FragCoord` reads (0,0) in fragment shaders that declare the SPIR-V float-controls execution mode | `gfx/dxvk/prepare.py`: `d3d11.floatControls` defaults to false on the Qualcomm proprietary driver |
| 10 | wrong initial texture data | `tex_subres_count` read MipLevels/ArraySize at wrong offsets | fixed; failed `Create*` calls are logged |
| 11 | host: texture creation fails during track load | DXVK budgets 80 % of the reported heap, lavapipe reports 2 GB | `TM_DXVK_NO_HEAP_BUDGET=1` (host only) |

**Performance.** Measured in-race at one spot of track #01 (`tools/device_fps.sh`,
SurfaceFlinger frame history; `TM_GFX_STATS=300` agrees). The GPU was 16 % busy
throughout: the limit was the one "virtual CPU" that the machine lock gives all guest
threads together. `simpleperf` on the device (`tools/device_perf.sh`) named every item.

| Step | fps | What was wrong |
|---|---|---|
| start | 19 | |
| guest state in one object (`RECOMP_CPU_STRUCT`, `RECOMP_CPU_TLS_SLOT=2`), hashed dispatch (`RECOMP_FAST_DISPATCH`) | 27–28 | 30 % of the main thread ran the linker's `tlsdesc_resolver_dynamic`: in a dlopen'ed library every `__thread` access is a resolver call, and a lifted call touches about forty registers/flags. The state is now one `recomp_cpu_t` per thread, found through bionic's `TLS_SLOT_APP`. Indirect calls took a binary search over 200k entries, and every import call two failed ones first. `libtmturbo.so` shrank from 758 to 653 MB |
| D3D11 Map: flush dynamic vertex/index buffers per draw call | 31 | the engine appends to a 1 MB dynamic vertex buffer about a hundred times per frame and `Unmap` copied the whole buffer each time: 42 MB per frame, 16 % of the main thread. Now each draw copies the vertex/index range it uses (0.5 MB per frame); static index buffers keep a host copy so the range is known |
| VP8 decoder and engine-sound synthesiser outside the machine lock (`runtime/src/parallel.c`) | ~95 | the decoder of the track-side video billboards (36 % of all CPU samples) and `audiomotors_process` (15 %) are self-contained number crunching that the PC runs on other cores; under the machine lock they took more than half of the virtual CPU from the main thread (32 %). They now run as "free regions"; imports called from inside take the lock for that call |
| present with vsync and a 60 fps cap (`TM_SYNC_INTERVAL`, `TM_MAX_FPS`, `TM_REFRESH_HZ`) | 60, steady | the game presents "as fast as possible"; 60–95 rendered frames on a panel that switches between 60 and 120 Hz gave uneven pacing and a hot device (78 °C). Now a frame takes 16.4–16.7 ms (median = p90), main thread at ~70 % of a core, GPU 44 % |

Also in this round: waits use one condition variable per waiting thread with targeted
wake-ups (every `SetEvent`/`LeaveCriticalSection` used to wake all ~10 blocked guest
threads), `LeaveCriticalSection` signals only a contended section, the machine lock hands
over FIFO with one wake-up; x87 `fist/fistp` store the x86 "integer indefinite" for
out-of-range values instead of the host's undefined cast (x86-64 and arm64 differ);
`-fsigned-char` for the runtime on arm64. Switches for A/B tests: `TM_PARALLEL=0`,
`TM_MAP_LAZY=0`.

**Tools added** (all under `tools/`): `device_race.sh` (launch → running race),
`device_steps.sh` (timed inputs with a screenshot each), `device_video.sh` (screen
recording → contact sheet; the way to see what an input does), `device_fps.sh`,
`device_perf.sh` (simpleperf record/report through `run-as`), `device_peek.sh` (live
guest memory), F11 = guest memory dump with `memgrep.py` / `memtext.py` (the game's
ManiaScript and manialink sources are plain text in memory — that is how the pre-race
logic was read), F12 = one-frame render capture with `cap2png.py`.

**Rebuild notes.** The header change needs a full rebuild of the lifted code on both
targets: Android 22 minutes with `JOBS=11` on the idle 16-core box, host (-O0, `-j12`) about 5; the rebuilt host was run as a regression (boot, intro, menus, track #01 up to the pre-race panel, `work/run31.log`). The
pcrecomp changes are exported as `tools/patches/pcrecomp/0005-*.patch`.

**Verified on the device with APK 0.4.0** (scripted gamepad input, screenshots and screen
recordings under `work/dev-*.png`): launcher start without extras (the game now starts
by itself when the files are present), tracks #01, #02 and #03, pause menu (RESUME /
GIVE UP / NEXT TRACK / QUIT with d-pad and A), QUIT back to the track grid and loading
another track in the same session, restart with B, two soak loops of 4 and 8 minutes
(accelerate, crash, restart) with constant memory (1.84 GB PSS) and no runtime warnings.

**Pause and resume.** The activity no longer ends the process when its surface goes
away. `onPause` closes a gate in front of the game's `Present` call (`tm_wsi_pause`, the
caller has released the machine lock) and pauses the OpenAL device; the surface is
dropped, and when a new one arrives DXVK rebuilds its Vulkan surface and swap chain on
it (the window token the swap chain holds stays the same). Tested: HOME and back from
the launcher, sleep / wake / unlock — the same race continues, the process uses 0 % CPU
in between. The activity is `singleTask` and starts the game once per process.
`--ez exit_on_background true` restores the old behaviour.

**Open.**
- Finish a race (needs real steering, not adb key events), check that medals/progress
  are saved and survive a relaunch. `Profile.Gbx` is written each session.
- Longer soak of the free regions (they rely on the decoder and the synthesiser sharing
  nothing with other threads except through OS calls; the soak loops above are clean).
- Remaining CPU headroom: lifted code is built at -O1; `lock`-prefixed instructions are
  still plain C under the machine lock (atomic lifting would allow more parallelism).
- Unimplemented imports that only matter if called: avifil32 (video capture),
  vorbis/ogg encoder, `ReadConsole`.

---

## 2026-11-10 (afternoon) — the "sun glare" in the middle of the screen

**Symptom (reported after playing):** depending on the camera a white, sun-like glare
sits in the middle of the picture; at the start of campaign track #01 it covers the start
lights in about two frames out of three, so it also flickers.

**Cause.** Not the game's lighting and not the lifted code: the Adreno 830 driver
(512.800) draws some passes wrongly when several render passes that share attachments
but have different attachment lists are recorded into one command buffer. The frame
contains

    {velocity}  ->  clear HDR  ->  {HDR, velocity} (247 draws)  ->  {HDR}  ->  {HDR, velocity} (6 draws: the lamps)

and when all of this is one submission, the lamps of the fourth pass come out so bright
that the bloom turns them into a disc. One submission boundary anywhere between the
first and the fourth pass removes it completely (0 of ~230 frames instead of ~160).
It only appeared with the performance work: before that the CPU needed 50 ms per frame
and DXVK's timer-based implicit flush (at least 750 µs since the last one, checked at
every render-target change) split the frame at most target changes anyway.

**How it was found.** Every readback or wait hides it, including the F12 frame capture,
so the usual tools showed a correct frame. What worked:
1. an automatic detector (`tools/device_disc.sh`: restart the race, record six seconds,
   count the frames whose start-light region is saturated; ~70 % with the bug);
2. a per-frame probe of the sprite constants — stable, so the wrong pixels are produced
   on the GPU side;
3. `TM_GFX_SYNC` modes switched at run time through `setprop debug.tm.gfxsync`: a Flush at
   every render-target change fixes it (and so the bug is "several passes per
   submission"), then bisecting — by index window first, which smeared because the number
   of passes varies from frame to frame, then relative to the pass structure
   (mode 12: "the n-th pass after the velocity pass"), which gave the clean result above;
4. a one-frame trace of the resource traffic (F10) for the pass structure.

Ruled out on the way: the draw-time vertex flush, the parallel regions, the wake-up
changes, the frame cap, timestamp queries, the uptime clock base, x87 integer stores,
occlusion queries (the game issues none), `UpdateSubresource` (none per frame), sampling a
bound render target (none), a transfer command between the passes (no effect), a Flush
between passes that keep their target list (no effect).

**Fix.** `rp_split()` in `gfx/d3d11_bridge.cpp` (`TM_RP_SPLIT`, default 1 on Android)
flushes in two situations: (a) a texture is bound as render target in a different slot
than it last had in this frame — the velocity target moving from slot 0 to slot 1,
between the first and the second pass above; (b) a list of several targets is bound
again after a different list was used in between — the fourth pass. Rule (a) alone still
left 1–2 % of the frames wrong. Together that is four to six flushes per frame instead
of ninety, and the frame rate is unchanged (59.9 fps, 16.4 ms median and p90).

Measured at the start of track #01 with `tools/device_disc.sh` (frames with a saturated
start-light region): rule off 127 of 239 (53 %), rule on 0 of 955 in four runs. A
22-second drive through the first section shows no glare and one brightness jump (the
scene cut at the restart) where the same drive had 53 before. Track #02's start looks
the same with the rule on and off.

`TM_RP_SPLIT=2` (Flush before every multi-target pass that follows a different target
list, about 21 per frame, about 3 fps) and `=3` (every real target change, 48 fps) are
the wider nets if another scene shows the same class of error;
`setprop debug.tm.rpsplit <n>` switches while the game runs (`setprop debug.tm.rpsplit ""`
afterwards, or the value stays until the device reboots).

**Side findings.**
- The engine calls the CRT's `atan2` and `exp` with NaN arguments thousands of times per
  second (guest functions 0x00BC0B30, matrix to Euler angles, and callers of 0x00EF4530).
  Found with the new `TM_FIST_LOG` (pcrecomp patch 0006, `RECOMP_FIST_HOOK`: out-of-range
  x87 integer stores call back into the host). Where the NaNs come from is open; nothing
  visible is tied to it yet.
- The uptime clocks (`QueryPerformanceCounter`, `GetTickCount`, `timeGetTime`) now start
  60 s before the game instead of at device boot (`TM_TIME_BASE=boot` restores that).
- The Odin3 is shared with other projects' sessions; a measurement is only valid if the
  game still has the foreground afterwards.

---

## 2026-11-10 (evening) — APK 0.5.0: a lifter bug behind the engine sound, a third less code, wider render-pass split

Reported after more play: menus and cutscenes still feel slow, the engine sound is partly
wrong, and in the air or with turbo most of the picture flickers — later narrowed down by
the player to water and particles.

**Lifter bug: `sar` on 8- and 16-bit operands shifted in zeros.** The lifter emitted
`(int32_t)<operand> >> n` for every width; a narrow operand reads as an unsigned value,
so the shift was logical. 599 sites in the game and its DLLs. The one that mattered most
is the MSVC CRT's x87 operand classification, `fxam; fnstsw; shl cl,1; sar cl,1;
rol cl,1; xlatb`: the `sar` carries C3, so without it every ZERO operand of
`atan2 / pow / fmod / exp ...` was taken for an unsupported number and the function
returned NaN with an "invalid" exception. That code exists in the game's CRT and again in
`audiomotors_shared_mt.dll` (the engine-sound synthesiser — the "weird" engine sound) and
it produced the NaN flood in the telemetry angles found earlier. About 540 more sites are
16-bit arithmetic in the bundled codecs (0x013B8000–0x014C3000). Fixed in `lift32.py`
(pcrecomp patch 0007, with `cwd` now writing DX only); 15 + 14 + 2 + 1 generated files
changed. Check: `TM_FIST_LOG=1` printed thousands of out-of-range x87 stores per second
in a race before and prints none now; a 234 s capture of the game's audio (OpenAL "wave"
backend, `tools/wav_check.py`) has no non-finite samples, no clipping, no drop-outs and
no clicks.

**Speed: the lifted code itself.** A profile of the attract-mode replay (about 1000
draws per frame, 42–56 fps before) put 88 % of the main thread in lifted game code, flat
over hundreds of functions (`tools/perf_agg.py`). The disassembly showed why: after
every store to guest memory the compiler reloaded `g_mem_base`, and before every
register write-back it reloaded the state pointer — it cannot know that neither is
changed by a guest store. `RECOMP_LOCAL_STATE` (same patch; `runtime/include/guest_prelude.h`
defines it and ends with `#include "recomp_local_state.h"`) keeps both in locals of each
lifted function. The hottest function went from 362 to 234 instructions and
`libtmturbo.so` from 653 to 433 MB.

Also: staging textures that the engine fills through `Map(WRITE)` (every video frame,
the particle emitter table; 7–17 maps per frame) made DXVK synchronise with its command
thread each time. The bridge already holds their contents, so it now maps them as
DISCARD (`TM_STAGING_DISCARD=0` to turn off).

| | 0.4.1 | 0.5.0 |
|---|---|---|
| attract-mode replay of a saved run | 42–56 fps, up to 30 % repeated frames | 60.0 fps, 0 repeated frames in 28 s |
| main menu / campaign menu / track grid | not measured | 59.6–60.0 fps |
| race, frame cap off | about 95 fps | 111–119 fps |
| race with the 60 fps cap | 60 fps | 60.0 fps, median = p90 = max = 16.7 ms |

**Render-pass split, wider.** The passes the player suspected sit exactly on the kind of
pass sequence that produced the glare: the cloud sprites render to {HDR, mask}, the
water meshes then write the same mask alone, the water surface reads it together with a
refraction copy; the particle simulation is a pair of two-target passes fed by a table
that is uploaded every frame (shader names from the captured DXBC). `TM_RP_SPLIT=1` now
flushes (a) on a slot change as before and (b) before every pass with several colour
targets whose target list differs from the previous one — 22 flushes per frame instead
of 4–6, still 60 fps (GPU 42–47 % busy). The glare test at the start of track #01 stays
at 0 of 239 frames. Not yet confirmed by measurement: that the water and particle flicker
is gone. No scripted scene shows it, and the detectors flag only camera cuts and camera
shake in the replay with the rule on and off alike; this needs the player's eyes.

**How the uneven frame rate looked.** The replay could be watched hands-free
(`tools/device_replay.sh`: with a saved ghost the pre-race attract mode replays it).
Below 60 fps the queue shows some frames twice; with the engine's motion blur that reads
as flicker at speed. At a steady 60 the recording has no repeated frames.

**Working on a shared device.** The player uses the same Odin3. Launching, installing or
force-stopping is only done when the screen has shown the same idle menu for ten minutes;
screen recordings, profiles (`simpleperf` attaches to the running game) and the
`debug.tm.*` properties work without interrupting a session.


## 2026-11-11 (night) — APK 0.5.1: the tile lattice over water

**Symptom** (player's screenshot, track #01, in the long jump after the turbo pads): most
of the picture one flat colour, the rest seen through a checkerboard of 32x32-pixel tiles;
in motion "most of the picture flickers". In a recording the flat areas turn out to be
screen-filling triangles and rectangles in changing colours and textures, different in
every frame, 70–100 bad frames in the 4 s the car is over the canyon.

**Reproduction without hands.** The glitch needs the race camera high over the canyon;
the attract replay (same place, cinematic cameras) never shows it in 170 s. Injected
`input` events are too jittery to steer (the first bend needs +-0.05 s), so the runtime
got a pad macro: `setprop debug.tm.padmacro "GAS:500:32000,LEFT:7400:7650,RIGHT:8300:8700,LEFT:9500:9800"`
starts when B (restart race) goes down and is applied at pad-poll time
(`runtime/src/input.c`). `tools/device_jump.sh <name>` restarts, records and counts the
bad frames with `tools/video_tiles.py` (a tile is "bad" when it is flat and three of its
four neighbours differ along the whole edge; 0 hits in 10 200 replay frames, 59 in the
player's screenshot). `tools/device_macro.sh` is the loop used to find the steering.

**What it is.** The tiles are the GPU's own: tile index (row * 64 + column) modulo 3
selects them — the Adreno 830 renders with three "slices". The triangles are the lake's
water surface (four big quads) drawn *through* the terrain: in those tiles the water
pass ignores the depth buffer. Ruled out with switches: lazy buffer flush
(`TM_MAP_LAZY=0`: 22–47 bad frames), staging-texture discard (`TM_STAGING_DISCARD=0`:
83–102), parallel regions, car skeleton (a new float watch on the vertex-stage constant
buffers, `debug.tm.fwatch`, reports no bad bone). `debug.tm.rpsplit 3` (a submission per
target change): 0 bad frames. So it is the render-pass class of the glare again.

Bisecting with `debug.tm.gfxsync "12 lo hi"` (flush when leaving the n-th pass after the
velocity pass) on the water sequence

    p45 {mask}+D  4 water meshes     <- the narrow rule already flushed before this
    p46 {HDR}+D   water fog, full screen
    p47 {960x540} refraction copy of HDR
    p48 {HDR}+D   4 water surfaces, then two emitter-table uploads
    p49 {particle state}  no draws
    p50 {state, state}    <- flushed before this

| extra flush | bad frames |
|---|---|
| none | 73–98 |
| p45 / p46 | 94 |
| p46 / p47 | 0 |
| p47 / p48 | 0 |
| p48 / p49 | 0 |

The last row is the odd one: the command stream is the same as without the extra flush,
only the boundary sits one (empty) pass earlier. DXVK's own timer flush lands in that
stretch in some frames, which is why the glitch flickers instead of staying.

**Fix.** `TM_RP_SPLIT` got two more modes (`rp_split()` in `gfx/d3d11_bridge.cpp`):
1 now also flushes when a target list *comes back* after something was rendered under
another list (rule c: the p47/p48 boundary), and 4 flushes at every target change that
has draws, clears or copies recorded before it. Since the driver's real condition is not
understood, 4 is the Android default.

| mode | flushes per frame | attract replay, cap off | jump: bad frames |
|---|---|---|---|
| 5 (0.5.0 rules a+b) | 22 | 104–112 fps | 79–98 |
| 1 (a+b+c) | 35 | 97–104 fps | 0, 0, 0 |
| 4 (default) | 56 | 98 fps | 0, 0 |
| 3 (every change) | 87 | 93 fps | 0 |

0.5.1 with its defaults (60 fps cap): the jump twice with 0 bad frames of 1680; a 40 s run
on through the landing in the lake and out over the sand, 0 of 2400; the attract replay
60.0 fps (median = p90 = max = 16.7 ms), 0 lattice and 0 flicker frames of 3000 (0.5.0
had one such frame in 28 s); the start-light glare test 0 of 240 frames, twice.
`work/water-lattice-before-after.png` shows the same moment with the 0.5.0 rules and with
the new default.

Other things learnt on the way: `tools/dxbc_dis.py` (rough DXBC disassembler with RDEF
names), `tools/shader_hash.py` (names a shader from the hash the float watch prints),
`tools/trace_passes.py` (one line per pass from an F10 trace). The engine's motion blur
(`MotionB_Tech3`) takes at most 6 taps on each side along the velocity; the car is
`CarAnimSkelDmg` with 40 bones of 3x4 floats in a 3808/3824-byte object buffer.

Sound in the same run (OpenAL Soft's wave backend, `ALSOFT_CONF` with `drivers = wave`):
253 s without a non-finite sample, clipping, drop-out or click; in the air the engine sits
at one steady pitch (the limiter) until the car lands in the lake. `TM_FIST_LOG=1` logged
no out-of-range float-to-int conversion in the whole session. Whether it *sounds* right
is still for the player to say.

## 2026-11-11 (midday) — APK 0.6.0: the menus' video ran at 6 frames per second

**Report:** "the menu is still slow, the animations in the menu are totally slow" and "the
turbo icon on the right flickers when it appears".

**Menu.** The main thread was never the problem: main menu, campaign menu, series page
and track grid present 59–60 frames per second (`TM_GFX_STATS=30`), and SurfaceFlinger's
own frame history shows a steady 16.7 ms with two or three 80–100 ms hitches when a page
is entered (`tools/device_latency.sh`). What is slow is the *video*. The menus are mostly
video: the sky with the helicopter behind the billboards is two 1920x1080 WebM streams,
the banner over the track grid and the logos at start-up are more. `tools/video_rates.py`
(changes per second for every 40x40 block of a recording) shows the background updating
6–12 times per second next to interface elements that update every frame; the bridge's
statistics show each 2 MB luma plane uploaded 6 times per second; and the profile of a
menu session (`tools/device_probe.sh`) has two decoder threads on top, ahead of the main
thread, in the lifted libvpx (`sub_013B5E10…sub_013CB042`: the six-tap predictors and
loop filters). The exe's libvpx 1.4 uses MMX/SSE2 routines, which the lifter turns into
scalar C: 60–110 ms per 1080p frame.

**Fix: a native decoder behind the game's libvpx** (`runtime/src/video.c`,
`tools/build_libvpx.sh`). libvpx reaches a codec through its interface table
(`vpx_codec_iface_t` at 0x019C2060, "WebM Project VP8 Decoder v1.4.0"), and those entries
are indirect calls, which overrides can replace:

| entry | VA | now |
|---|---|---|
| init, peek_si, get_si, controls | 0x0139BAC0 … | the game's own (they only touch its decoder object) |
| decode | 0x0139BE60 | native libvpx 1.14.1 (NEON), outside the machine lock |
| get_frame | 0x0139C280 | fills the game's `vpx_image_t` (layout read from its `yuvconfig2image`, 0x0139BCC0: it has `range`, `r_w`, `r_h`, so planes sit at +0x30 and strides at +0x40) with planes in guest memory |
| destroy | 0x0139BB20 | frees the native decoder, then the game's own |

Frames go round three guest buffers with libvpx's geometry (32-pixel border, 16-aligned),
because libvpx keeps a shown frame intact while it decodes the next one and a player may
rely on that. `TM_VIDEO_NATIVE=0` returns to the game's decoder, `TM_VIDEO_LOG=1` prints
frames per second and decode time per stream. The host build can link a generic libvpx
(`work/libvpx-host`, see `host/CMakeLists.txt`): there the start-up logos and the intro
film decode at 8–20 ms per 1080p frame on a busy machine and look right
(`work/hostvid-sheet.png`).

**Turbo icon: not a port bug.** The HUD script was read out of a memory dump taken
during a race (F11, `tools/memgrep.py`):

    if(GUIPlayer.EngineTurboRatio > 0) {
        Quad_Turbo.Visible = True;
        Quad_Turbo.Opacity = 0.85 * (0.5 + 0.5 * ML::Sin(Now * 0.1));

a sine with a period of 62.8 ms on an additive quad, i.e. a 16 Hz strobe. At 60 frames per
second that is 3.8 samples per period, which looks like irregular flicker: bright, dim,
gone, dim, bright. The brightness of the badge in 70 consecutive frames of the player's
own run fits that formula sampled every 16.66 ms. The original does the same at 60 fps;
only a faster display turns it into a smooth pulse.

**Tools from this round.** `tools/device_guard.sh` (scripts that inject input stop when
the device saw input that was not theirs: the player picked the device up twice while a
script ran), `tools/device_idle.sh`, `tools/device_menu.sh` (launch → series page with
recording, profile, statistics), `tools/device_probe.sh` (timed presses + recording +
profile from the current state), `tools/device_keys.sh`, `tools/device_latency.sh`,
`tools/video_updates.py`, `tools/video_rates.py`. `TM_FRAME_LOG=<ms>` reports frames whose
work took longer, split into waiting for the machine, file reads and object creation.
A screen recording drops frames when the device is busy: identical frames in a recording
are not proof of a stall, SurfaceFlinger's frame history is.

### Afternoon: what the player saw next (same APK number, 0.6.0)

**"The transition from the campaign to the track selection is far too slow."** It is a
video too: the flip of the board from the series page to the ten tracks plays
`TransitionBillboard/BillEnviro_Canyon.webm` (two 1400x1400 streams) and the script waits
for it. With the lifted decoder the flip took 5.5 s, with the native one 1.2 s (frame
sheets `work/tr-old-new.png`, `work/trans2-sheet.png`). On the device the native decoder
needs 2.8–3.3 ms for a 1080p or 1400x1400 frame and every stream runs at its 30 frames
per second (`TM_VIDEO_LOG=1`).

**"The turbo icon still flickers."** Since it is the game's own 16 Hz strobe (above), this
is now a deliberate deviation, `runtime/src/tweaks.c`, `TM_HUD_CALM` (default 1, 0 = as the
original): the script library's `Sin` (Execute at 0x0070BAA0, reached through the vtable at
0x016F9394) is wrapped, and a call whose argument advances by 0.1 per millisecond for
several frames in a row gets a slow pulse (2.4 Hz, 0..1) instead of the sine. Of the
40-odd sines in the game's scripts only the badge moves that fast; the detection
(`tweaks_detect.h`) was tested off the device with all of them at 20–120 fps
(`work/chk/calm_test.c`: 95–99 % of the badge's calls, none of the others). In the
player's next session the badge pulses at 2.5 Hz between about half and full brightness.

**"On track 2 there is a lens-flare bug again, like the sun at the start of track 1."**
That was 0.5.1's own doing. With a submission boundary at every render-target change
(`TM_RP_SPLIT` 3, 4 or 6) a white blob with the tile lattice inside appears over the
start line (first taken for the start lamps' glow; it is the sun's lens flare, see the
evening section). Measured with the car parked at the start line of track #05
(`tools/video_blob.py`, properties switched while the game ran):

| `TM_RP_SPLIT` | frames with the blob |
|---|---|
| 4 (0.5.1's default) | 20–54 % |
| 3, 6 | 41 %, 32 % |
| 0, 2, 5 | 0 of 481 each |
| 1 (rules a+b+c) | 0 of 8102 |

So a boundary is not harmless either, and "flush more" is not the safe direction. The
default is 1 again: the narrowest rules that remove every known case. With them on the
final build: track #05 start line 0 of 8102 frames, start lamps of track #01 0 of 240
(twice), the water jump 0 of 1680 (twice), 60.0 fps.

One more source of boundaries was left: DXVK flushes by itself at a render-target change
when 750 us have passed since the last flush, i.e. at places that depend on timing. In
the player's sessions with rule 1 three single white frames showed up in about 15 600; with
that flush switched off (live, `debug.tm.implflush 0`) none in 23 000, at the same frame
rate. Not a controlled comparison (different driving), but a timing-dependent boundary is
the only thing that can make a single frame differ, so `gfx/dxvk/prepare.py` now puts that
one flush under `g_tm_dxvk_implicit_flush` and the bridge turns it off on Android
(`TM_DXVK_IMPLICIT_FLUSH=1` brings it back; the query and busy-resource flushes stay).

**Input guard.** The power manager's "last user activity" also moves when a window gains
focus (our own launches, a track loading), so `tools/device_guard.sh` now reads the age of
the last key/motion event from `dumpsys input` instead.

### Evening: what the blob on track #05 is, and exactly which boundary makes it

**Report:** "on track 5 you see the lens flare issue right at the start."

**A hypothesis that did not hold.** The engine tests the sun's visibility with an
occlusion query (`LensFlareOccQuery`: a point at the sun's position), so the first idea
was a query result read too early on this driver. Logged every result
(`TM_QUERY_LOG=4`): it is 1 in every frame, with and without the blob, and a DXVK patch
that held results back until their command buffers had finished changed nothing
(23 % / 41 % / 35 % of the frames with the blob). Patch removed again.

**What it is.** The flare shader (`2dFlareAdd_Hdr`) scales the flare by
`TMapCloudsOcc`, a 2x2 R16F texture. The one-frame trace shows where that comes from:
`CopySubresourceRegion(2x2 <- cloud mask 1920x1080)` at the sun's position, right before
the flare is drawn. The cloud mask is the second target of the cloud-sprite pass
`{HDR, mask}` (cleared to 1 = "no cloud"). On track #05 the sun stands low behind
clouds at the start, so the flare should be almost invisible; when the mask reads
"no cloud" there, the full flare is drawn: the white blob.

**The boundary.** `tools/trace_splits.py` replays the split rules on a traced frame and
shows that modes 1 and 4 differ, around these passes, only in whether the submission ends
right after the cloud pass. Adding single boundaries to rule 1 (`debug.tm.gfxsync
"12 n n"`, car parked at the start line, 1800 frames each):

| extra boundary | frames with the blob |
|---|---|
| none | 0 |
| after the cloud pass `{HDR, mask}` (before the `{HDR}` fog pass) | 37 % |
| after the water-mask pass, after the fog pass | 0 |
| before the flare pass | 0.4 % |
| after the flare pass | 0 |

So a submission that ends with that two-target pass loses part of its second target in
about a third of the frames (one of the GPU's three slices, again). Modes 3, 4 and 6 end
a submission there; rule 1 keeps the following `{HDR}` pass in the same submission.
Under rule 1 a two-target pass still ends a submission when another two-target list
follows directly (the particle-state passes): nothing wrong has been seen there, but it
is the place to look if particles ever misbehave.

**Timing-dependent boundaries removed.** Besides DXVK's timer flush (afternoon), the
bridge itself flushed whenever the game polled a query whose result was not ready yet
(0.03-0.1 times per frame, in the middle of the flare stage). `TM_QUERY_FLUSH` (default
1) now flushes only when the same query is polled again within a frame, i.e. when the
game really spins on it; 2 = as before. With both changes every boundary follows from
the frame's structure alone.

**Final 0.6.0 (installed 16:43), measured with its defaults:**

| scene | frames with a fault |
|---|---|
| track #05 start line, parked | 0 of 4803 |
| track #05 countdown, 3 restarts (7 more on the build before, same settings) | 0 of 1621 (0 of 2162) |
| track #05, parked 30 m past the line (sun in view) | 0 of 12005 |
| the same with `TM_RP_SPLIT=4` (control) | 23-41 %, countdown 32 % |
| track #01 start lamps | 0 of 239, 0 of 229 |
| track #01 water jump | 0 of 1680 |

60.0 fps throughout (36 splits per frame). The single white frames seen in the player's
sessions on the interim build (3 in 15 600) could not be reproduced any more, with the
interim settings either (0 of 7202), so their cause is not proven.

New tools: `tools/trace_splits.py`, `tools/occ_log.py`, `tools/video_blob.py`,
`tools/device_track.sh`; the F10 trace now lists query Begin/End/GetData.

## 2026-11-11 (late evening) — the community tracks

**Request:** "Trackmania Turbo has a huge library of custom maps: make them available in
the game."

**Where they are.** Players shared their Track Lab tracks through Ubisoft's servers; the
in-game browser ("Trackbuilder Workshop") needs an account and those services. Ubisoft's
portal https://players.turbo.trackmania.com/pc/maps still lists them (40 per page;
about 58 000 PC tracks, 1 865 of them played at least 500 times), and the map files are
public on the portal's S3 bucket next to the thumbnails:
`https://tmturbo-prod-pc-maps.s3.amazonaws.com/<mapUid>.Map.Gbx`.
`tools/ugc_fetch.py list|fetch|pack` indexes the portal (one page per second), downloads
the files and packs them for the device. 199 of the popular ones are Nadeo's own uploads
(the campaign) and are left out; the other 1 666 (Canyon 1 247, Stadium 267, Lagoon 99,
Valley 53; 905 MB) are on the device.

**How the game lists tracks.** TRACKBUILDER -> MANAGE YOUR TRACKS ("My Tracks") calls
`DataMgr.RetrieveUserFiles(MenuUserId, "", Map)`, which lists the profile's own folder
`Documents\TrackmaniaTurbo\<profile>\MapsGhosts` through its index `FileList.Gbx`
(class 0x031B7000, chunk reader at 0x009ACD00: entries of file name, FILETIME, size, kind;
kind 0 = map with uid and name, kind 1 = ghost with name, uid and time; LZO1X-compressed
body). Maps that are only in the folder, or in `Documents\TrackmaniaTurbo\Maps\My Maps`
(which the game scans at start-up for other purposes), are not shown. Without the index the
game crashed at start-up (null pointer in 0x00773CE0); file names in the index must be
shorter than 32 characters.

**Import.** `runtime/src/tracks.c`: at start-up every map in `<user dir>/ImportTracks`
(the one place adb and file managers can write to) whose uid the profile does not have is
copied into MapsGhosts as `<n>.Map.Gbx` (from 1000 up) and added to `FileList.Gbx`; the
existing entries (the player's tracks and best-time ghosts) stay untouched -- reading and
rewriting the current index gives a byte-identical body (`work/chk/tracks_test.c`). The
first import keeps the original index as `FileList.Gbx.before-import`. File names
`<rank> <title> <author>__<uid>.Map.Gbx` give the order and the label shown in the list
("#107 Mr.DvD_TM"). `TM_TRACKS_IMPORT=0` skips the step.

On the device with the first 8 tracks: they appear under My Tracks with thumbnail,
environment, "Validated", length and style; ACTION -> PLAY loads the track, the race starts
and runs (track #107 by Mr.DvD_TM, `work/mt7-s.png`).

**Mishaps on the way.** A test that moved the index aside made the game crash at start-up,
and the scripted presses that followed went to the launcher, which started another app.
`tools/device_guard.sh` now presses only while the game has the focus (and retries while
the game switches surfaces); the profile was repaired from the backup, verified identical.

Tools: `tools/ugc_fetch.py`, `tools/gbx_body.py` (decompresses a GBX body for inspection),
`tools/device_mytracks.sh` (launch -> My Tracks), `tools/memlines.py`; `TM_FS_LOG=1` logs the
game's directory searches and map/index opens; the LAN receiver also serves files (GET).

**All 1 666 on the device (21:20).** The import of the whole set takes 1.3 s at start-up
(the files are copied once; later starts only read the index). The list is sorted by
popularity (the order of the import), but the game shows each track's own name ("107",
"4", ...), not the label in the index. Opening My Tracks reads every listed map first:

| tracks listed | until the list appears |
|---|---|
| 8 | about 2 s |
| 400 | 18 s |
| 800 | 35 s |
| 1 666 | about 80 s |

i.e. about 44 ms per track, every time the page is opened. The main thread is only about
60 % busy meanwhile (the rest of the CPU goes to the menu videos), so the loading looks
paced by frames; a measurement without the frame cap was cut short because the player
picked up the device. `TM_TRACKS_MAX=<n>` lists only the n most played (the others are not
copied, or removed again).

## 2026-11-11 (night) — APK 0.7.0: mouse and touch, the ManiaPlanet bar, quieter OpenAL

**"Engine sound partly choppy again."** The game sets two source properties of Nadeo's own
OpenAL build (0x1034/0x1035, `AL_DIRECT_MATRIX71_SOURCECHANNELS_SOFT` / `_VALUES_SOFT`;
it enables them because `AL_SOFT_direct_channels` exists) about 850 times per second.
OpenAL Soft rejects each one, and on Android it writes every error to logcat regardless of
`ALSOFT_LOGLEVEL`: about 1 700 log lines per second from the game's audio path. The bridge
(`tools/gen_openal_bridge.py` → `audio/openal_bridge.c`) now drops those two properties in
all `alSource*` / `alGetSource*` calls. After the fix: 0 such lines; during the water-jump
run on track #01 the game's output track in `dumpsys media.audio_flinger` shows 0 underruns
(48 kHz, 432-frame buffer). The player's session before had also been my uncapped test
launch (no 60 fps cap), which can have added to it.

**"The proper options."** On the PC the in-game SETTINGS page is the console one (music,
voices, ranking zone, Speed-I/O-Meter, Trackbuilder lighting, credits); graphics, input and
sound are in the ManiaPlanet bar that slides in when the mouse cursor touches the top edge
of the screen. The port had no mouse at all. Now:
- `runtime/src/input.c` `tm_input_mouse()`: the cursor as WM_MOUSEMOVE / WM_xBUTTONDOWN/UP /
  WM_MOUSEWHEEL in client coordinates, `GetAsyncKeyState(VK_xBUTTON)`, and the DirectInput
  mouse (`dinput8.c`: immediate DIMOUSESTATE2 and buffered events; the game registers the
  standard 20-byte format with 11 objects and reads the buffered data 120 times a second).
- The game hides the Windows cursor and moves its own by the DirectInput motion from a
  start of its choosing, so when the cursor jumps (a new touch) the motion first runs into
  the top-left corner (the game clamps its cursor to the window) and then to the touch.
- `MainActivity`: the game surface turns touches into the cursor: the cursor moves at once,
  the left button goes down 70 ms later and stays down at least 70 ms. Without that delay
  the menus saw move, press and release in one frame and ignored the click (the press was
  tested against the control under the old cursor position). A connected mouse works
  directly (hover, buttons, wheel).

Verified on the device: a tap at the top edge shows the bar (volume, gear, clock, window
buttons); the gear opens GRAPHIC / INPUT / REPLAY EDITOR; GRAPHIC opens "Graphic
settings" (display mode, resolution, antialiasing, filtering, water reflections,
ADVANCED..., APPLY / CANCEL) -- `work/topbar-graphic.png`. Taps also select and open the
game's own menu entries (SETTINGS, CAMPAIGN, a track on the grid). Whether the graphics
settings actually take effect through DXVK was not tested.

`TM_MOUSE_LOG=1` logs the game's mouse data format, its read pattern, the mouse events it
takes, SetCursorPos / ShowCursor and mouse window messages. `tools/device_keys.sh` has
`tap:x:y` and `move:x:y` steps.

## 2026-11-12 — APK 0.9.0: game data from the player's own folder, start screen, LAN games, C++ exceptions

The user asked for: no game data in the APK (import the player's own game folder like a
proper recomp, with a clean menu), and local multiplayer across several devices.

### Import and start screen (done, verified on the Odin3)

- `GameImporter.java`: the player picks the folder of their PC installation (Storage Access
  Framework tree) or a ZIP of it. Copied are only what the game needs (`GameData/**`,
  `Packs/**`, the exe, the DLLs, `nadeo.ini`); the exe is copied first and checked against the
  SHA-256 of this port's build (2016-11-07_16_15), with a clear German message otherwise;
  free space is checked (+200 MB); files go to `.part` first; same-size files are skipped
  (re-import); at the end `.import-ok` records the hash and totals. Test: 54 files / 4.14 GB
  from `/sdcard/Download/Trackmania Turbo` in under 8 s (UFS), the game ran from the copy to
  the title screen. The ZIP path was not tested on the device.
- `Launcher.java` (start screen built in code, touch + mouse + pad: D-pad focus, A select,
  B back): SPIELEN, LAN-SPIEL, SPIELDATEN (NEU) IMPORTIEREN, EINSTELLUNGEN (frame rate, turbo
  badge, community tracks, start log), BEENDEN. The game no longer autostarts
  (`--ez autostart true` still skips the screen for scripts; `--es game_dir NAME` imports
  into / runs from another folder).

### LAN: hosting works, joining does not yet

What the engine has: ManiaPlanet's whole LAN stack. Command line `/server`, `/join=`,
`/lan`, `/nickname=` (parser 0x00890F10); XML-RPC StartServerLan / JoinServerLan
(handlers 0x008B1610 / 0x008B13A0); the native menus "Local Network – Choose a server /
Create a server" (they appeared on the x86 build, see below); the title has the online
mode scripts `Modes\TrackMania\TMC_OnlineTimeAttack|Rounds|Laps.Script.txt`.

Host (`TM_LAN=host`, `runtime/src/lan.c`, `net.c`): 
- `net.c` with `TM_NET_LAN=1`: private ranges + broadcast reachable, binds keep the
  requested address, the host name is per device (player name + address: it is the
  player's login, a server refuses duplicates), `GetAdaptersInfo` reports the Wi-Fi adapter
  (`misc.c`). Offline play is unchanged (loopback only).
- `/server /lan` makes the app start a server; without maps it stops with "No maps in the
  playlist match the server settings". lan.c wraps the playlist check (0x00E61090,
  CTrackManiaNetwork vtable 0x0183B8A8 +0x278) and loads the MatchSettings file first
  (vtable +0x27C LoadMatchSettings, 0x00EA7DF0; Utf8ToWide 0x0074F470). The start screen
  writes `Maps\MatchSettings\LanHost.txt` (mode, campaign tracks by colour/environment:
  `Campaigns\0c_Colour\0e_Env\NNN.Map.Gbx`, time per track). 
- Without "server ports" the server is local only (one UDP client socket, nothing listens):
  lan.c sets net+0x218 (CNetSystem config at net+0x204, +0x14) in the virtual call right
  before the app switches the network on (0x00E62570, vtable +0x1A4), so 0x0052AD40 opens
  UDP+TCP 2350 (port from the system config, default 0x92E). Verified: the host races on
  its server, TCP 2350 listens, other clients reach it and get the server info.
- The start screen announces a hosted game on UDP 47350 once a second (`Lan.java`), the
  join page lists what it hears.

C++ exceptions (`runtime/src/eh.c`, needed by any network game): the engine throws
`CNetSystem::CNetSystemError` for every socket error and every connection the other side
closes (0x0051A460 recv, ...), caught a few frames up; without SEH dispatch that ended the
game. Now: the 109 functions with try blocks (`config/eh_wrap.txt`, from
`work/eh_funcs.py`: FuncInfo -> handler thunk -> function pushing it) are lifted as a stub
`tm_eh_wrap(va, body)` that sets a host setjmp anchor (`tools/run_lift.py`, 29 files
changed); RaiseException(0xE06D7363) walks the guest SEH chain like __CxxFrameHandler3,
runs the unwind funclets of the left frames and of the try block, builds the catch
object, calls the catch funclet (-> continuation, all continuations are catalog entries),
destroys the exception object, and longjmps to the anchor, which resumes the lifted code
at the continuation with esp = [frame-0x10], ebp = node+0xC. Verified: "eh:
CNetSystemError thrown at 00EF1E38, caught in 0052A160 (handler 0052A313)", the host runs on.

Join (`TM_LAN=join`, `TM_LAN_JOIN=ip:port`): NOT WORKING YET.
- `/join=` on the command line is handled before the client has loaded its title.
  lan.c therefore waits until the client's title is loaded (CGameCtnApp::HeavyUpdate
  0x008A4440 is virtual; title = net+0x26C vtable+0x160 -> CMwId, 0x005035A0 GetName:
  'TMTurbo@nadeolabs' on host and client) and then does what JoinServerLan does (net+0x15C
  = 1, address +0x160, password +0x170, +0x178 = 1, +0x17C silent, +0x180 = 1).
- The client then queries the server (A14D60 -> A159E0 -> 9C17D0 -> 9CA0E0: TCP connect,
  sends 90 bytes, gets 635 bytes of server info; the native "JOIN / SPECTATE / CANCEL"
  dialog shows name, mode TMC_ONLINETIMEATTACK, 40 maps, ping), and the reply handler
  0x009C1820 (class 0x12010000 sub 5) closes that connection because it is in the query
  list (net+0x1E0). On JOIN the connect step 0x009C0AE0 passes the title check but finds
  no server connection ([[net+0x264]+0x34] via 0x00532360 / 0x00531D10) and fails ->
  "Could not connect." With the client's own ports open: "Could not contact server."
  The join coroutine 0x00A14D60 checks for an existing connection first; what should set
  it up in a LAN join is still open.
- The native LAN lobby: on the x86 build, `/server /lan` plus a network restart during the
  server start (`TM_LAN_RESTART=1`, development only) lands in "Local Network – Choose a
  server" (players on the LAN are listed both ways, discovery broadcasts to UDP
  2350..2400) with CREATE -> "Server parameters" -> LOAD SETTINGS (finds LanHost.txt) ->
  LAUNCH; LAUNCH did nothing in the test (maybe an empty game name: the native text fields
  need WM_CHAR, which the test input does not send). This engine-native path may be the
  better way for both sides.

Test setup for this (no second device needed): `work/hosthost.sh` / `work/lanmenu.sh` /
`work/hostjoin.sh` run the x86 build as host / LAN-lobby / client in Docker (same bridge,
broadcasts work), `TM_INPUT_FILE` takes live clicks (`click:x:y` in 1/1000), `TM_NET_TRACE=1`
logs every socket call, `TM_LAN_DEBUG=1` the client's connection fields, `work/join.gdb` +
`work/hostjoin_gdb.sh` give guest call chains (host backtraces are the lifted functions).

### 2026-11-12 (later): LAN join, three layers further

- **Hook sites** are usable now: `config/hooks.txt` -> the lifter emits `RECOMP_HOOK(va)` before
  that instruction (registers synced), `runtime/src/core.c` dispatches to handlers registered
  with `tm_hook_register(va, fn)`; a handler may read and change guest registers. Relift only
  the touched files (lift into work/gen-hk, copy the changed ones; 1–2 files per change).
- **A join needs a master server user.** 0x009C0AE0 (the join step, mode 1 of the network
  state machine 0x009C5580) takes the current user of the master server client
  (net+0x264, class derived from CNetMasterServer, vtable 0x017F3E40): 0x00532360 →
  [[ms+0x34]+0x10] = user id, 0x00531D10 → user object from the list ms+0x28/+0x2C.
  Offline there is none → "Could not connect". lan.c makes one with the engine's
  AddUser(&id, makeCurrent) 0x00531DB0 right before the lookup (hook site 0x009C0BE8; the
  app's user sync 0x008B7DA0 → 0x005398A0 drops it again and again, the account list at
  [0x01C1B5AC]+0x1C holds pointers, do not write an id there — that crashed).
  With it the client sends the real join request (0x11DBF70(addr, 1, 1, msg, 1), 73 bytes,
  partly plain: "ManiaPlanet", "394", the login).
- The host would verify the login's signature (0x009CBC00 → "Corrupted login.", keys via
  0x00531CC0 / 0x00418BF0); hook site 0x009CBD45 lets it pass on a LAN host. Not reached yet:
- **Open:** the host cannot read the join request. TcpReceive (0x00521630) → message
  parser 0x0051E2F0: type 01 like the (readable) query messages, then the type's body
  reader [entry+0x14] = 0x00548E70 → 0x005484E0 returns 0 (failed after the check at
  0x0051EB13) → CNetSystemError "CNetConnection::TcpReceive(2)" → connection closed →
  client: "Could not connect: Connection to …:2350 closed by server". Likely the 16 bytes
  after the 0x80 0x03 header are a signature bound to the master server session.
- Test notes: the x86 build is too slow as a *host* (its server times out connections,
  "CNetServer::Poll(4)" = 0x0052A220); use the Odin3 as host and the x86 build as client
  (`TM_LAN_DEBUG=1` there also stretches its info wait to 60 s). eh.c now logs the
  CNetSystemError text and likely return addresses.

### 2026-11-12 (night): LAN join works — the client gets into the server's game

What the host could not read was not a signature but a **cut message**: the request's
writer had stopped half-way. Fixed in lan.c, step by step (each found from the bytes on
the wire, `TM_NET_TRACE=1`, now also received bytes and datagrams; `=2` dumps all bytes):

- **Join request** (class vtable 0x01806C98, ctor 0x00C54940, built in 0x009C0AE0 from
  0x009C0C15; reader/writer 0x00C55150, kind 0 subtype 3 — the game replaces the generic
  callbacks 0x01B43C04.. at 0x009BCD7D): base strings "ManiaPlanet", "394", host name; then
  the login (+0x4C, 1..[0x01BF33E4]=46 chars), password hash (+0x58, 16 bytes), mode byte
  (+0x70, 1..3), version strings +0x74 (net+0x1FC, 1..10 chars, "6"), +0x7C ([net+0xBB4]+0x58,
  10..30 chars, "v2016-11-07_16_15 (3.3.0)"), +0x84 build string, wide strings +0x8C/+0x94/
  +0x9C, a 32-byte block +0xA8, byte +0xA4. The writer checks the same bounds and returns
  0 at the first violation, leaving whatever it wrote so far in the message: our login was
  empty (length byte 0), so the host's reader failed right there.
- **The login of the offline master server user.** 0x00555DC0 gives it as CFastString
  +0x1C XOR key CFastString +0x24 (length +0x28); the plain copy is +0x14. lan.c sets both
  (login = player name lower-cased [a-z0-9_-] + "_" + last IP byte, e.g. `odin3_177`).
- **Join mode**: arg 2 of 0x009C0AE0 (= the join coroutine's state +0x20, from net+0x178
  or the play/spectate dialog 0x00A1693C) must be 1..3; the hook at 0x009C0BE8 makes 0 → 1.
- **Ban "internal checks (2) failed for '<login>'"** (+ "You were banned." in the host's
  last message): the host's player-info reader 0x009BAC40 compares the login in the
  client's player info with the connection's (check 0, "login may be forged") and wants it
  non-empty (check 2); failures go through the queue 0x01C1961C/0x01C19628 (0x009BBA70) to
  the ban in 0x009C80B0. The client's local player info is net+0xBB4 (login CFastString
  +0x30, normally copied from user+0x14 by 0x008884C0); lan.c sets it.
- **Nickname**: the local player info's CFastStringInt +0xC keeps the ctor's default
  "Unnamed" (0x009B9F90) offline; lan.c sets it from `TM_PLAYER_NAME` on host and client
  (the host shows "ODIN3 HAS JOINED THE ROOM").
- **Script crash on entering the race** (x86 client, SIGSEGV at guest 0x28 in 0x005126D0):
  the playground UI script reads `InputPlayer.User.Echelon` while InputPlayer is still NULL;
  the reflection walk (0x00505680 → member accessor vtable+0x94 = 0x005126D0) does not
  check the owner. New runtime/src/guards.c wraps 0x005126D0: a field address in the NULL
  page returns 1, which the script VM turns into "Error during access to parameter"
  (0x0071F225), a script error instead of a crash.

Verified: Odin3 host ⇄ x86 client: join accepted, map and skins loaded, past the former
crash point, client keeps exchanging TCP/UDP messages with the racing host. x86 host
(ports published: `HOST_PUBLISH=1 work/hosthost.sh`) ⇄ Odin3 client: joined, the host shows
"ODIN3 HAS JOINED THE ROOM", the Odin3 shows the server's map with "WAITING".
Not verified yet: two players actually racing together. The x86 build is a poor stand-in
for that: under lavapipe it compiles shaders for minutes (host: answers queries / the UDP
setup only once its race runs, else the client gives up with "UDP initialization failed";
client: black screen for minutes), and its 2 GB device heap ran out once while loading.
A test with two Android devices is the next step. `TM_MUTE=1` (dev) keeps a test session
on the shared device silent.

Later the same night:
- **Why a joined client stays at "WAITING"** with the x86 host: after joining, the client
  synchronises its clock with the server over UDP (24-byte messages, class 0x03069000, type
  7, body reader 0x008D6970); the server's handler 0x009CC250 only answers while its own
  frame time is under 300 ms (`[0x01BF1CF8] < 0x12C` and now − `[0x01BF1CF0]` < 0x12C,
  time structure 0x01BF1CE8). The x86 host (lifted code at -O0, lavapipe) is far slower, so
  it never answers; the client never leaves synchronisation and finally drops out
  ("disconnected from server") when the server's match ends. An Android host at 60 fps
  meets the limit. The host parses the client's UDP fine (sequential connection ids), and
  UDP through Docker's published ports works both ways (checked with work/udp_echo.py).
  For a usable PC stand-in an -O1 x86 build is being made (work/build-host-o1;
  `HOST_BUILD=work/build-host-o1` for work/hosthost.sh and work/hostjoin.sh).
- **Join retries** (lan.c `join_failed`): a join that fails before the client got in is
  tried again up to 6 times, 8 s apart — "Could not connect…" (error flag net+0x1D4) and
  "could not contact" (no answer to the info query, hook site 0x00A16700; while attempts
  remain the hook sets [ebp+0x10] = 2 so the coroutine ends quietly instead of waiting on
  its message box). Verified with the x86 client against a closed port: attempts 1..6, the
  last one shows the game's message.
- APK 0.11.0 (versionCode 14) installed on the Odin3 with all of this.
- **The Odin3 races as a LAN client.** With a PC host that answers the clock sync (x86
  build at -O1 — work/build-host-o1 — rendering at 480x270, `TM_HOST_SCREEN=480x270
  TM_WINDOW_FILL=1`, plus the dev override `TM_LAN_SYNC_ALWAYS=1` that makes its frame look
  fresh while it handles a message), the Odin3 joined the running match and its own car was
  spawned on the server's map ("ODIN3 HAS JOINED THE ROOM", match clock running, the host
  streaming ~10 UDP datagrams/s; work/lan-client11-s.png). Without the override the -O1 host
  still misses the 300 ms limit (lavapipe) and the client stays in synchronisation.
- At the end of that match the x86 host hung (one core at 100 %, no more log output,
  probably loading the next map) and the client dropped out; the x86 client earlier showed
  the same kind of hang. **x86 only:** the Odin3 as host goes through the match change
  (0:11 → "Congrats!" podium → next map "Canyon Grand Drift", "The match is now open";
  work/hostseq-11-13.png).
- Still open: two Android devices racing together through a match change (no second fast
  device in the rig); a join that fails with "Could not connect." leaves the game's
  message box up, which may hold up the automatic retry until A is pressed.
- New dev switches: `TM_HOST_SCREEN=WxH` (host/main_host.c), `TM_LAN_SYNC_ALWAYS=1` and
  `TM_LAN_CLOCKLOG=1` (lan.c, host), `HOST_BUILD=` / `HOST_PUBLISH=1` / `TM_HOST_SCREEN` in
  work/hosthost.sh.

### 2026-11-13: two Android devices in one LAN game (Odin3 + iPlay60 mini Turbo)

- iPlay60 mini Turbo (Snapdragon 6 Gen 1, Adreno 710, driver 0615.84, Android 14): APK
  0.11.0 + game data over the LAN file server (tools/lan_serve.py + `--es import_url`, 4.1 GB
  in ~2 min). It first crashed right after joining (0x007D1F40, NULL read in the renderer):
  DXVK gave it D3D feature level 9_2 (Odin3: 11_0) because its Vulkan driver lacks some of
  the features DXVK 1.9 wants for 9_3..11_0, and the game's low-end path crashed.
  gfx/dxvk/prepare.py now makes those (independentBlend, multiViewport,
  shaderImageGatherExtended, dualSrcBlend, imageCubeArray, drawIndirectFirstInstance,
  fragmentStoresAndAtomics, multiDrawIndirect, tessellationShader, transform feedback)
  optional: enabled when present, no longer deciding the level; a GPU lacking some logs
  "tm: GPU lacks optional features: …". iPlay: level 11_0, no crash.
- Odin3 host + iPlay client: joined on the first attempt, both in the same match (match
  clocks agree), each shows the other's name tag, both go through the match change to the
  next map together, the iPlay drives (pad macro). "Invalid time: the race of X will be
  ignored" (native, 0x00E63C8A → message class 0x24030000 subtype 6) is the client reporting
  a race it did not finish properly: the flag net+0x1148 is set at start-up and by a restart
  (0x00ECD0A0) and outside warm-up the next round start reports it. Seen once after joining
  and after each B restart — expected behaviour, not a LAN fault.
- The user took over both devices for a first hands-on test (sessions were started muted,
  `TM_MUTE=1`, Odin3 with the test profile user-b).

## 2026-11-13 (evening) — 120 Hz, second round (APK 0.12.0)

The system setting is 120 Hz now (`peak_refresh_rate=min_refresh_rate=120`). Measured on
track #01 with the jump macro (work/race_fps3.sh; `TM_GFX_STATS`, DXVK_HUD, SurfaceFlinger):

| change | main-thread frame work |
|---|---|
| before (render thread on a 3.53 GHz core, 41 submissions/frame) | 13.0 ms |
| render thread bound to the prime cores (cpu6/7, 4.32 GHz) + ADPF hint session (`TM_PRIME`, `TM_ADPF`) | 9.0 ms |
| DXVK `MaxNumQueuedCommandBuffers` 18 → 96 (the CS thread waited for the GPU every frame) | 7.0 ms |
| render-pass splits as a full memory barrier instead of a Flush (`tm_dxvk_barrier`, `TM_RP_BARRIER`) — 2 submissions/frame instead of 41 | 4.9 ms |

The barrier keeps the Adreno water lattice away just as the submissions did: 0 of 1680
frames over the jump (tools/device_jump.sh), 23 of 1680 without any splits.

**Heat decides the rest.** Android caps the display at 60 Hz once the device is warm
(`PRIORITY_SKIN_TEMPERATURE` vote; Odin3 "skin" sensor thresholds 48/49/50/60 °C, thermal
status MODERATE). After a few minutes of testing (and while charging) the skin sensor reads
50–61 °C, so most of today's measurements were capped. A game that keeps asking for 120 then
gets every third refresh = 40 fps; the app now follows `PowerManager` thermal status and
asks for 60 while hot (clean 60.0 fps, measured), 120 again when cool. GPU load in the race
is ~50 % at 60 fps, i.e. about the 120 fps budget: heavy scenes will run at the adaptive
cap's 60.

**Decision (user): stay at a stable 60 fps.** APK 0.12.1: the start screen's "Bildrate"
defaults to 60 ("120 fps (120-Hz-Display, Gerät wird warm)" stays selectable); the render
thread binds to the prime cores only for targets above 60 (a 60 fps frame needs ~5 ms on
the mid cores, which run cooler); the ADPF hint (target 16.7 ms) stays. The work above
gives the 60 fps cap a large margin (frame work ~5 ms of 16.7).

## 2026-11-12 — 120 fps

- **Display:** the Odin3 panel has a 120 Hz mode (id 1) and a 60 Hz mode (id 2). The app now
  asks for the mode that fits the "Bildrate" setting (MainActivity.requestRefreshRate:
  `preferredDisplayModeId` + `Surface.setFrameRate(fps, FIXED_SOURCE)`), but the system's
  `peak_refresh_rate` is 60 on this device and its vote
  (PRIORITY_USER_SETTING_PEAK_REFRESH_RATE, max 60) beats the app's request
  (PRIORITY_APP_REQUEST_BASE_MODE_REFRESH_RATE 120) — the panel stays at 60 Hz until the
  user sets the display to 120 Hz in the Android settings. Not changed by us.
- **Setting:** "Bildrate" 120 (default) / 60 / 30 → TM_MAX_FPS.
- **Measured** (uncapped: TM_MAX_FPS=0, TM_SYNC_INTERVAL=0, TM_FPS_ADAPT=0; new stats line
  "frame work" = end of one Present to the start of the next, avg and p95): menus ~145–150
  fps (6.6–6.9 ms, p95 7.5–9 ms); race track #01 ~75–80 fps (12.4–14 ms, p95 16–24 ms),
  GPU 62–67 % at 900 MHz. The race is CPU-bound on the main thread (prime core): 87 % lifted
  game code spread over ~160 files (hottest function 3.5 %), DXVK's CS thread ~63 % of a
  core. -O2 for the 40 hottest generated files (config/hot_files.txt, CMake GUEST_HOT_OPT)
  gave no measurable gain (device thermally throttled during these runs: zones 95–105 °C,
  prime cores 3.8 of 4.32 GHz) → back to -O1 by default.
- **Adaptive cap** (gfx/d3d11_bridge.cpp, TM_FPS_ADAPT=1 default when the cap is above
  60): full rate while 90 % of the frames (windows of 120) fit the budget, else half of it
  (every frame exactly two refreshes: no 1/2-refresh alternation that the engine's motion
  blur turns into flicker) for at least 300 frames, back once 120 frames in a row fit with
  10 % to spare. Not yet seen on a 120 Hz panel (needs the user's display setting).
- Small: indirect-call resolution checks a 4096-bit filter before scanning the override
  list (core.c, was 2–3 % of the main thread).
- To reach 120 in races, the main thread needs ~40 % less work: lifter-level ideas
  (per-call-site caches for RECOMP_ICALL, fewer register syncs around calls) or lower
  in-game graphics settings (fewer draws/scene work) — not done yet.
