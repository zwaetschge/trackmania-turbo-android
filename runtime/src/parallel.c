/* Guest work that runs OUTSIDE the machine lock.
 *
 * Guest threads are serialised by the machine lock (lifted `lock`-prefixed
 * instructions are plain C, and x86 code may lean on its strong memory ordering).
 * Two helper threads of this game, though, spend their time in self-contained
 * number crunching that the original runs on other cores, and under the machine
 * lock they took more than half of the one "virtual CPU" away from the main
 * thread (in-race profile, Odin3):
 *   - the VP8 decoder of the track-side video billboards and menu backgrounds
 *     (libvpx inside the exe): 36 % of all CPU samples, more than the main thread;
 *   - the engine sound synthesiser, audiomotors_process() in
 *     audiomotors_shared_mt.dll: 15 %.
 * Both work on their own state and buffers and reach other threads only through
 * operating system calls. A "free region" releases the machine lock for the
 * duration of such a function; every import called from inside takes it again
 * for that call (core.c: recomp_resolve), so shims still never run concurrently.
 * TM_PARALLEL=0 turns all of it off. */
#include "tmrt.h"
#include <stdlib.h>
#include <string.h>

static int g_enabled = -1;
static int enabled(void) {
    if (g_enabled < 0) { const char *e = getenv("TM_PARALLEL"); g_enabled = !(e && *e == '0'); }
    return g_enabled;
}

/* vp8_decode (the decoder's vpx_codec_iface "decode" entry, reached through the
 * interface table, i.e. by an indirect call that overrides can intercept). */
#define VA_VP8_DECODE 0x0139BE60u
static recomp_func_t g_vp8_decode;
static void free_vp8_decode(void) { tm_free_region(g_vp8_decode); }

static recomp_func_t g_am_process;
static void free_am_process(void) { tm_free_region(g_am_process); }

/* Called once the image is mapped. */
void tm_parallel_register(void) {
    if (!enabled()) { TM_INFO("parallel regions disabled (TM_PARALLEL=0)"); return; }
    if (tm_video_native()) return;                /* video.c replaced the decoder altogether */
    g_vp8_decode = recomp_lookup(VA_VP8_DECODE);
    if (g_vp8_decode) tm_add_override(VA_VP8_DECODE, free_vp8_decode);
}

/* Lifted-module exports pass through here when they are registered. */
recomp_func_t tm_parallel_wrap_export(const char *dll, const char *name, recomp_func_t fn) {
    if (!enabled()) return fn;
    if (!strcmp(name, "audiomotors_process") && !g_am_process) { g_am_process = fn; return free_am_process; }
    (void)dll;
    return fn;
}
