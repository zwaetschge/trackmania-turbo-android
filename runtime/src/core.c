/*
 * core.c - guest register file, machine lock, dispatch, diagnostics,
 * host->guest calls, handle table and the shared sync primitive.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define __USE_GNU 1
#include <dlfcn.h>
#include <unistd.h>

/* ------------------------------------------------ guest machine state */
#ifdef RECOMP_CPU_STRUCT
/* One state object per thread (see recomp_types.h). With RECOMP_CPU_TLS_SLOT the
 * lifted code finds it through a word of the thread control block; that word is
 * set the first time a thread takes the machine lock (tm_cpu_bind). */
#ifdef RECOMP_CPU_TLS_SLOT
static __thread recomp_cpu_t t_cpu = RECOMP_CPU_INIT;
static __thread char t_cpu_bound;
void tm_cpu_bind(void) {
    if (t_cpu_bound) return;
    if (recomp_cpu_get()) TM_WARN("thread control block slot %d already holds %p", RECOMP_CPU_TLS_SLOT, (void *)recomp_cpu_get());
    recomp_cpu_set(&t_cpu);
    t_cpu_bound = 1;
}
#else
RECOMP_TLS recomp_cpu_t g_cpu = RECOMP_CPU_INIT;
void tm_cpu_bind(void) {}
#endif
#define t_holds (*(intptr_t *)&RECOMP_CPU->c_host[1])
#else
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
RECOMP_TLS int      g_fp_top;
RECOMP_TLS uint16_t g_fpu_cw = 0x027F;
RECOMP_TLS uint32_t g_fs_base, g_gs_base;
RECOMP_TLS uint64_t g_mm[8];
RECOMP_TLS V128     g_xmm[8];
RECOMP_TLS uint32_t g_mxcsr = 0x1F80;
RECOMP_TLS uint32_t g_cur_func;
RECOMP_TLS uint32_t g_icall_trace[ICALL_TRACE_SIZE];
RECOMP_TLS uint32_t g_icall_from[ICALL_TRACE_SIZE];
RECOMP_TLS uint32_t g_icall_trace_idx;
RECOMP_TLS uint32_t g_icall_count;
static __thread int t_holds;
void tm_cpu_bind(void) {}
#endif
uint16_t g_seg_cs = 0x23, g_seg_ds = 0x2B, g_seg_es = 0x2B, g_seg_fs = 0x53, g_seg_gs = 0x2B, g_seg_ss = 0x2B;
ptrdiff_t g_mem_base;
#ifdef RECOMP_TRACE
RECOMP_TLS uint32_t g_enter_trace[RECOMP_ENTER_SIZE];
RECOMP_TLS uint32_t g_enter_idx;
void recomp_trace_enter(uint32_t va) { g_enter_trace[g_enter_idx++ & (RECOMP_ENTER_SIZE - 1)] = va; }
#endif

volatile int g_guest_exit_requested;
int g_guest_exit_code;
tm_platform_hooks g_platform;

/* ------------------------------------------------ logging */
int tm_log_level = TM_LOG_INFO;
void (*tm_log_sink)(int level, const char *line);
void (*tm_status_sink)(const char *line);
void (*tm_fatal_hook)(const char *why);
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

void tm_log(int level, const char *fmt, ...) {
    if (level > tm_log_level) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mu);
    if (tm_log_sink) tm_log_sink(level, buf);
    else { fprintf(stderr, "[tm:%c:%u] %s\n", "EWIT"[level & 3], tm_thread_id(), buf); fflush(stderr); }
    pthread_mutex_unlock(&g_log_mu);
}

void tm_status(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tm_log(TM_LOG_INFO, "STATUS %s", buf);
    if (tm_status_sink) tm_status_sink(buf);
}

void tm_dump_state(const char *why) {
    if (!tm_cpu_bound()) { TM_ERR("==== %s on a host thread without guest state ====", why); return; }
    TM_ERR("==== guest state (%s) thread %u ====", why, tm_thread_id());
    TM_ERR("eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X fs=%08X",
           g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp, g_fs_base);
    TM_ERR("current function %08X, icalls %u", g_cur_func, g_icall_count);
    char line[1024]; int n = 0;
    uint32_t idx = g_icall_trace_idx;
    int shown = 0;
    for (int i = 1; i <= 24 && i <= (int)idx; i++) {
        uint32_t k = (idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = tm_import_name(g_icall_trace[k]);
        n += snprintf(line + n, sizeof line - n, " %08X<-%08X%s%s", g_icall_trace[k], g_icall_from[k], nm ? ":" : "", nm ? nm : "");
        if (++shown % 4 == 0 || n > 700) { TM_ERR("icall ring:%s", line); n = 0; line[0] = 0; }
    }
    if (n) TM_ERR("icall ring:%s", line);
    if (g_esp) {
        n = 0;
        for (int i = 0; i < 16; i++) n += snprintf(line + n, sizeof line - n, " %08X", rd32(g_esp + 4 * i));
        TM_ERR("stack:%s", line);
    }
    /* Dump the object each pointer register points at (first 20 dwords), to see
     * corrupted this/arrays at a crash. */
    struct { const char *nm; uint32_t v; } ptrs[] = {{"ebx", g_ebx}, {"esi", g_esi}, {"edi", g_edi}, {"ecx", g_ecx}};
    for (unsigned p = 0; p < 4; p++) {
        uint32_t base = ptrs[p].v;
        if (base < 0x10000u || base >= 0xF0000000u) continue;
        n = 0;
        for (int i = 0; i < 20; i++) n += snprintf(line + n, sizeof line - n, " %08X", rd32(base + 4 * i));
        TM_ERR("[%s=%08X] hdr{sz=%08X cls=%08X mg=%08X}:%s", ptrs[p].nm, base,
               base >= 16 ? rd32(base - 16) : 0, base >= 16 ? rd32(base - 12) : 0, base >= 16 ? rd32(base - 8) : 0, line);
    }
#ifdef RECOMP_TRACE
    n = 0;
    for (int i = 1; i <= 32; i++) n += snprintf(line + n, sizeof line - n, " %08X", g_enter_trace[(g_enter_idx - i) & (RECOMP_ENTER_SIZE - 1)]);
    TM_ERR("enter ring:%s", line);
#endif
}

void tm_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    TM_ERR("FATAL: %s", buf);
    tm_status("Stopped: %s", buf);
    tm_dump_state(buf);
    fflush(stderr);
    /* The platform layer may keep the process alive to show the stop. */
    if (tm_fatal_hook) tm_fatal_hook(buf);
    _exit(70);
}

void recomp_unimpl(uint32_t va, const char *what) {
    tm_fatal("unimplemented x86 instruction at %08X: %s", va, what);
}

/* Collect mode (TM_ICALL_COLLECT=<path>): log every unresolved indirect target
 * to a file and continue (eax=0) instead of stopping, so one run surfaces all
 * functions the disassembler missed. They are then added to the catalog and the
 * code re-lifted once. Default (strict) stops with a diagnostic. */
void recomp_unresolved(const char *kind, uint32_t va, uint32_t from) {
    static FILE *collect; static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER;
    static int checked;
    if (!checked) {
        pthread_mutex_lock(&cmu);
        if (!checked) { const char *p = getenv("TM_ICALL_COLLECT"); if (p) collect = fopen(p, "a"); checked = 1; }
        pthread_mutex_unlock(&cmu);
    }
    if (collect) {
        pthread_mutex_lock(&cmu);
        fprintf(collect, "%08X %08X %s\n", va, from, kind); fflush(collect);
        pthread_mutex_unlock(&cmu);
        g_eax = 0;
        return;
    }
    tm_fatal("%s to unresolved guest VA %08X from function %08X", kind, va, from);
}

/* Hook sites (config/hooks.txt -> RECOMP_HOOK(va) before that instruction, registers
   synced): handlers registered by the runtime (tm_hook_register); they may read and change
   the guest registers. */
#define MAX_HOOKS 128
static struct { uint32_t va; void (*fn)(uint32_t); } g_hooks[MAX_HOOKS];
static int g_nhooks;
void tm_hook_register(uint32_t va, void (*fn)(uint32_t)) {
    if (g_nhooks < MAX_HOOKS) { g_hooks[g_nhooks].va = va; g_hooks[g_nhooks].fn = fn; g_nhooks++; }
}
void recomp_hook(uint32_t va) {
    for (int i = 0; i < g_nhooks; i++) if (g_hooks[i].va == va) g_hooks[i].fn(va);
}
void recomp_dump_trace(const char *why) { tm_dump_state(why); }

/* ------------------------------------------------ dispatch */
/* Extra dispatch table for functions added after the main catalog (indirect-only
 * targets the disassembler missed). Weak so a build without extras links. */
__attribute__((weak)) extern const recomp_dispatch_entry_t extra_recomp_dispatch_table[];
__attribute__((weak)) extern const uint32_t extra_recomp_dispatch_count;
static recomp_func_t lookup_table(const recomp_dispatch_entry_t *t, uint32_t n, uint32_t va) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t mid = (lo + hi) >> 1; uint32_t a = t[mid].address;
        if (a == va) return t[mid].func; if (a < va) lo = mid + 1; else hi = mid; }
    return NULL;
}
/* Lifted bundled DLLs. Weak: a build without a module still links, and the
 * main image's imports of that DLL then stop with "unimplemented import". */
#define TM_WEAK_MODULE(pfx) \
    __attribute__((weak)) extern const recomp_dispatch_entry_t pfx##_recomp_dispatch_table[]; \
    __attribute__((weak)) extern const uint32_t pfx##_recomp_dispatch_count;
TM_WEAK_MODULE(am)
TM_WEAK_MODULE(wp)
const tm_lifted_module tm_lifted_modules[] = {
    { "audiomotors_shared_mt.dll", 0x10000000u, am_recomp_dispatch_table, &am_recomp_dispatch_count },
    { "libwebp.dll",               0x10100000u, wp_recomp_dispatch_table, &wp_recomp_dispatch_count },
    { NULL, 0, NULL, NULL }
};

static recomp_func_t lookup_slow(uint32_t va) {
    recomp_func_t f = lookup_table(recomp_dispatch_table, recomp_dispatch_count, va);
    if (f) return f;
    if (&extra_recomp_dispatch_count && extra_recomp_dispatch_table) {
        f = lookup_table(extra_recomp_dispatch_table, extra_recomp_dispatch_count, va);
        if (f) return f;
    }
    for (const tm_lifted_module *m = tm_lifted_modules; m->dll; m++)
        if (m->count && m->table && va >= m->base && va - m->base < 0x00100000u)
            return lookup_table(m->table, *m->count, va);
    return NULL;
}

/* Indirect calls are frequent (every virtual call, every import), and a binary
 * search over ~200k entries misses the cache at most of its 18 steps. One open-
 * addressing hash table over all dispatch tables answers in one or two probes.
 * Main-table entries are inserted first, so on a duplicate VA they win, as in
 * the search order above. */
static recomp_dispatch_entry_t *g_disp_hash;
static uint32_t g_disp_mask;
static pthread_once_t g_disp_once = PTHREAD_ONCE_INIT;
static inline uint32_t disp_slot(uint32_t va) { return (va * 0x9E3779B1u) >> 12; }
static void disp_insert(const recomp_dispatch_entry_t *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!t[i].func || !t[i].address) continue;
        uint32_t h = disp_slot(t[i].address) & g_disp_mask;
        while (g_disp_hash[h].func && g_disp_hash[h].address != t[i].address) h = (h + 1) & g_disp_mask;
        if (!g_disp_hash[h].func) g_disp_hash[h] = t[i];
    }
}
static void disp_build(void) {
    uint32_t n = recomp_dispatch_count;
    if (&extra_recomp_dispatch_count && extra_recomp_dispatch_table) n += extra_recomp_dispatch_count;
    for (const tm_lifted_module *m = tm_lifted_modules; m->dll; m++) if (m->count && m->table) n += *m->count;
    uint32_t cap = 1024;
    while (cap < n * 2u + 16u) cap <<= 1;
    recomp_dispatch_entry_t *h = calloc(cap, sizeof *h);
    if (!h) return;                               /* lookups stay on the binary search */
    g_disp_hash = h; g_disp_mask = cap - 1;
    disp_insert(recomp_dispatch_table, recomp_dispatch_count);
    if (&extra_recomp_dispatch_count && extra_recomp_dispatch_table) disp_insert(extra_recomp_dispatch_table, extra_recomp_dispatch_count);
    for (const tm_lifted_module *m = tm_lifted_modules; m->dll; m++) if (m->count && m->table) disp_insert(m->table, *m->count);
}

recomp_func_t recomp_lookup(uint32_t va) {
    if (__builtin_expect(!g_disp_hash, 0)) {
        pthread_once(&g_disp_once, disp_build);
        if (!g_disp_hash) return lookup_slow(va);
    }
    uint32_t h = disp_slot(va) & g_disp_mask;
    for (;;) {
        const recomp_dispatch_entry_t *e = &g_disp_hash[h];
        if (e->address == va && e->func) return e->func;
        if (!e->func) return NULL;
        h = (h + 1) & g_disp_mask;
    }
}

/* Manual overrides: host replacements for specific guest functions. */
#define TM_MAX_OVERRIDES 256
static struct { uint32_t va; recomp_func_t fn; } g_overrides[TM_MAX_OVERRIDES];
static int g_override_count;
/* Every indirect call asks whether its target is overridden; a 4096-bit filter over the
   override addresses answers "no" without scanning the list (2-3 % of the main thread). */
static uint64_t g_override_bloom[64];
#define OVR_BIT(va) (((va) >> 2) & 4095u)
void tm_add_override(uint32_t va, recomp_func_t fn) {
    if (g_override_count < TM_MAX_OVERRIDES) {
        g_overrides[g_override_count].va = va; g_overrides[g_override_count].fn = fn; g_override_count++;
        g_override_bloom[OVR_BIT(va) >> 6] |= 1ull << (OVR_BIT(va) & 63);
    }
}
recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_override_count; i++) if (g_overrides[i].va == va) return g_overrides[i].fn;
    return NULL;
}

/* The single resolver behind RECOMP_FAST_DISPATCH. Import bridges live in their
 * own address range, so they are recognised without touching the tables. */
/* An import called from a free region (see tm_free_region): the shim runs under
 * the machine lock like every other shim. */
#ifdef RECOMP_CPU_STRUCT
#define t_pending_shim (*(recomp_func_t *)&RECOMP_CPU->c_host[2])
#else
static __thread recomp_func_t t_pending_shim;
#endif
static void shim_relocked(void) {
    recomp_func_t f = t_pending_shim;
    tm_machine_lock();
    f();
    tm_machine_unlock();
}
static inline recomp_func_t import_for_caller(recomp_func_t f) {
    if (f && __builtin_expect(!t_holds, 0)) { t_pending_shim = f; return shim_relocked; }
    return f;
}
recomp_func_t recomp_resolve(uint32_t va) {
    recomp_func_t f;
    if ((g_override_bloom[OVR_BIT(va) >> 6] >> (OVR_BIT(va) & 63) & 1) && (f = recomp_lookup_manual(va)) != NULL) return f;
    if (va >= TM_SHIM_BASE) return import_for_caller(recomp_lookup_import(va));
    if ((f = recomp_lookup(va)) != NULL) return f;
    return import_for_caller(recomp_lookup_import(va));
}

/* Run a lifted function outside the machine lock: self-contained computation
 * that the original program runs on another core (parallel.c lists them and
 * says why that is sound). Nested use and callers that do not hold the lock
 * simply call through. */
void tm_free_region(recomp_func_t fn) {
    if (!t_holds) { fn(); return; }
    tm_machine_unlock();
    fn();
    tm_machine_lock();
}

/* ------------------------------------------------ x87 integer stores out of range
 * The lifted fist/fistp calls this when the value does not fit (or is NaN). The
 * x86 answer is the "integer indefinite" (0x80000000 / 0x8000000000000000).
 * TM_FIST_SATURATE=1 gives the saturating arm64 conversion instead (NaN -> 0),
 * which is what the port did before the conversion was made exact; TM_FIST_LOG=1
 * names the guest functions that convert such values. */
int64_t recomp_fist_oor(double r, int bits) {
    static int mode = -1, logging;
    if (mode < 0) {
        const char *e = getenv("TM_FIST_SATURATE"), *l = getenv("TM_FIST_LOG");
        logging = l && *l && *l != '0';
        mode = e && *e && *e != '0';
    }
    if (logging) {
        /* One line per distinct host call chain (the lifted callers; symbolise the
         * offsets with llvm-symbolizer on the unstripped libtmturbo.so), then its
         * count at every power of two. */
        static struct { uint64_t key; uint32_t n; } seen[64];
        static int nseen;
        uintptr_t ra[10]; int nra = 0;
#if defined(__aarch64__) || defined(__x86_64__)
        uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
        uintptr_t lo = (uintptr_t)&fp, hi = lo + (8u << 20);
        while (nra < 10 && (uintptr_t)fp > lo && (uintptr_t)fp < hi && !((uintptr_t)fp & 7)) {
            ra[nra++] = fp[1];
            uintptr_t *next = (uintptr_t *)fp[0];
            if (next <= fp) break;
            fp = next;
        }
#endif
        uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < nra && i < 6; i++) key = (key ^ ra[i]) * 1099511628211ull;
        int i;
        for (i = 0; i < nseen && seen[i].key != key; i++) { }
        if (i == nseen && nseen < 64) { seen[nseen].key = key; seen[nseen].n = 0; nseen++; }
        if (i < 64) {
            uint32_t n = ++seen[i].n;
            if (n == 1 || (n >= 256 && (n & (n - 1)) == 0)) {
                Dl_info di; uintptr_t base = dladdr((void *)recomp_fist_oor, &di) ? (uintptr_t)di.dli_fbase : 0;
                char line[400]; int k = 0;
                for (int j = 0; j < nra; j++) k += snprintf(line + k, sizeof line - k, " %lx", (unsigned long)(ra[j] - base));
                TM_INFO("x87 integer store out of range: %g -> int%d in guest function %08X (%u times), host chain:%s",
                        r, bits, tm_cpu_bound() ? g_cur_func : 0, n, line);
            }
        }
    }
    if (mode) {
        if (r != r) return 0;
        if (bits == 32) return r > 0 ? INT32_MAX : INT32_MIN;
        return r > 0 ? INT64_MAX : INT64_MIN;
    }
    return bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
}

/* ------------------------------------------------ machine lock (FIFO hand-over)
 * One guest thread runs at a time. Waiters queue in arrival order, each on its
 * own condition variable, and an unlock hands the machine to the first of them
 * with exactly one wake-up (a shared condition variable woke every waiter at
 * every release just to send all but one back to sleep). */
typedef struct tm_waiter {
    pthread_cond_t cv;
    struct tm_waiter *next;
    const void *const *keys;      /* sync waits: what the thread waits for; none = any wake */
    int nkeys;
    int granted;                  /* machine lock: ownership was handed to this waiter */
} tm_waiter;
static __thread tm_waiter t_waiter;
static __thread char t_waiter_init;
static tm_waiter *waiter_self(void) {
    if (!t_waiter_init) {
        pthread_condattr_t a; pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&t_waiter.cv, &a);
        t_waiter_init = 1;
    }
    return &t_waiter;
}

static pthread_mutex_t g_mach_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_mach_held;
static tm_waiter *g_mach_head, *g_mach_tail;

/* Diagnostics (TM_FRAME_LOG, see sc_Present in the bridge): per thread, the time it spent
 * waiting for the machine (kind 0) and inside host file reads (kind 1). */
static __thread uint64_t t_diag_ns[4];
static int g_diag = -1;
int tm_diag_on(void) {
    if (g_diag < 0) { const char *e = getenv("TM_FRAME_LOG"); g_diag = e && *e && atoi(e) > 0 ? atoi(e) : 0; }
    return g_diag;
}
void tm_diag_add(int kind, uint64_t ns) { t_diag_ns[kind & 3] += ns; }
uint64_t tm_diag_take(int kind) { uint64_t v = t_diag_ns[kind & 3]; t_diag_ns[kind & 3] = 0; return v; }

void tm_machine_lock(void) {
    if (!tm_cpu_bound()) tm_cpu_bind();           /* first guest activity of this host thread */
    if (t_holds) tm_fatal("machine lock re-entered");
    pthread_mutex_lock(&g_mach_mu);
    if (!g_mach_held) g_mach_held = 1;
    else {
        tm_waiter *w = waiter_self();
        w->granted = 0; w->next = NULL;
        if (g_mach_tail) g_mach_tail->next = w; else g_mach_head = w;
        g_mach_tail = w;
        uint64_t t0 = g_diag > 0 ? tm_now_ns() : 0;
        while (!w->granted) pthread_cond_wait(&w->cv, &g_mach_mu);
        if (t0) t_diag_ns[0] += tm_now_ns() - t0;
    }
    pthread_mutex_unlock(&g_mach_mu);
    t_holds = 1;
}

void tm_machine_unlock(void) {
    if (!t_holds) tm_fatal("machine lock released without being held");
    t_holds = 0;
    pthread_mutex_lock(&g_mach_mu);
    tm_waiter *w = g_mach_head;
    if (w) {                                      /* stays held: it now belongs to w */
        g_mach_head = w->next;
        if (!g_mach_head) g_mach_tail = NULL;
        w->granted = 1;
        pthread_cond_signal(&w->cv);
    } else g_mach_held = 0;
    pthread_mutex_unlock(&g_mach_mu);
}

static void tm_yield_hook(void) {
    /* Only hand over when another thread is actually waiting. */
    if (!t_holds) return;                         /* inside a free region: nothing to hand over */
    if (!g_mach_head) return;                     /* racy read is fine: checked again next time */
    tm_machine_unlock(); sched_yield(); tm_machine_lock();
}

__attribute__((constructor)) static void tm_core_ctor(void) { recomp_yield_hook = tm_yield_hook; }

/* ------------------------------------------------ host -> guest calls */
uint32_t tm_call_guest_v(uint32_t va, int nargs, const uint32_t *args) {
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_import(va);
    if (!fn) tm_fatal("host->guest call to unknown VA %08X", va);
    uint32_t saved_esp = g_esp, saved_ebp = g_ebp, saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi;
    uint32_t saved_func = g_cur_func;
    for (int i = nargs - 1; i >= 0; i--) { g_esp -= 4; wr32(g_esp, args[i]); }
    g_esp -= 4; wr32(g_esp, RECOMP_RETADDR);
    fn();
    uint32_t r = g_eax;
    g_esp = saved_esp; g_ebp = saved_ebp; g_ebx = saved_ebx; g_esi = saved_esi; g_edi = saved_edi;
    g_cur_func = saved_func;
    return r;
}

uint32_t tm_call_guest(uint32_t va, int nargs, ...) {
    uint32_t a[16];
    if (nargs > 16) tm_fatal("too many guest call args");
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return tm_call_guest_v(va, nargs, a);
}

uint32_t tm_call_guest_this(uint32_t va, uint32_t self, int nargs, ...) {
    uint32_t a[16];
    if (nargs > 16) tm_fatal("too many guest call args");
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t saved_ecx = g_ecx;
    g_ecx = self;
    uint32_t r = tm_call_guest_v(va, nargs, a);
    g_ecx = saved_ecx;
    return r;
}

/* ------------------------------------------------ sync */
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static tm_waiter *g_sync_waiters;     /* threads blocked in tm_sync_wait*, under g_sync_mu */

uint64_t tm_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void tm_sync_lock(void) { pthread_mutex_lock(&g_sync_mu); }
void tm_sync_unlock(void) { pthread_mutex_unlock(&g_sync_mu); }

/* Wake the threads waiting for `key` (an object, a critical section, a condition
 * variable); NULL wakes every waiter. Waiters that named no key take any wake.
 * Caller holds the sync mutex. Before this every SetEvent, ReleaseSemaphore and
 * LeaveCriticalSection woke all blocked guest threads (about ten of them, some
 * 2000 times a second each in a race) only for them to go back to sleep. */
int tm_sync_legacy = -1;          /* TM_SYNC_LEGACY=1 (diagnostics): every wake reaches every waiter, as before */
void tm_sync_wake(const void *key) {
    if (tm_sync_legacy < 0) { const char *e = getenv("TM_SYNC_LEGACY"); tm_sync_legacy = e && *e == '1'; }
    if (tm_sync_legacy) key = NULL;
    for (tm_waiter *w = g_sync_waiters; w; w = w->next) {
        int hit = !key || w->nkeys == 0;
        for (int i = 0; !hit && i < w->nkeys; i++) hit = w->keys[i] == key;
        if (hit) pthread_cond_signal(&w->cv);
    }
}
void tm_sync_broadcast(void) { tm_sync_wake(NULL); }

/* Caller holds the sync mutex. Returns 0 when the timeout expired. Spurious
 * returns are allowed; callers re-check their condition. */
int tm_sync_wait_keys(const void *const *keys, int nkeys, uint32_t ms, uint64_t start_ns) {
    struct timespec ts; uint64_t deadline = 0;
    int timed = ms != 0xFFFFFFFFu;
    if (timed) {
        deadline = start_ns + (uint64_t)ms * 1000000ull;
        uint64_t now = tm_now_ns();
        if (now >= deadline) return 0;
        ts.tv_sec = deadline / 1000000000ull; ts.tv_nsec = deadline % 1000000000ull;   /* tm_now_ns is CLOCK_MONOTONIC too */
    }
    tm_waiter *w = waiter_self();
    w->keys = keys; w->nkeys = nkeys;
    w->next = g_sync_waiters; g_sync_waiters = w;
    int r = timed ? pthread_cond_timedwait(&w->cv, &g_sync_mu, &ts) : pthread_cond_wait(&w->cv, &g_sync_mu);
    for (tm_waiter **pp = &g_sync_waiters; *pp; pp = &(*pp)->next) if (*pp == w) { *pp = w->next; break; }
    return timed && r == ETIMEDOUT ? (tm_now_ns() < deadline) : 1;
}
int tm_sync_wait(uint32_t ms, uint64_t start_ns) { return tm_sync_wait_keys(NULL, 0, ms, start_ns); }

/* ------------------------------------------------ handle table */
#define TM_MAX_HANDLES 65536
static tm_object *g_handles[TM_MAX_HANDLES];
static uint32_t g_handle_hint = 1;
static pthread_mutex_t g_handle_mu = PTHREAD_MUTEX_INITIALIZER;
/* Handles are 4-aligned small values like on Windows: (index << 2) + 0x100 */
#define H2I(h) ((((h) - 0x100u) >> 2))
#define I2H(i) (((uint32_t)(i) << 2) + 0x100u)

uint32_t tm_handle_new(tm_object *o) {
    pthread_mutex_lock(&g_handle_mu);
    for (uint32_t n = 0; n < TM_MAX_HANDLES - 1; n++) {
        uint32_t i = 1 + (g_handle_hint + n) % (TM_MAX_HANDLES - 1);
        if (!g_handles[i]) {
            g_handles[i] = o; o->refs++;
            g_handle_hint = i;
            pthread_mutex_unlock(&g_handle_mu);
            return I2H(i);
        }
    }
    pthread_mutex_unlock(&g_handle_mu);
    tm_fatal("handle table full");
}

tm_object *tm_handle_get(uint32_t h, int kind) {
    if (h < 0x100u || (h & 3)) return NULL;
    uint32_t i = H2I(h);
    if (i >= TM_MAX_HANDLES) return NULL;
    pthread_mutex_lock(&g_handle_mu);
    tm_object *o = g_handles[i];
    pthread_mutex_unlock(&g_handle_mu);
    if (!o || (kind && o->kind != kind)) return NULL;
    return o;
}

static void tm_obj_release(tm_object *o) {
    if (--o->refs == 0) {
        if (o->destroy) o->destroy(o);
        else free(o);
    }
}

int tm_handle_close(uint32_t h) {
    if (h < 0x100u || (h & 3)) return 0;
    uint32_t i = H2I(h);
    if (i >= TM_MAX_HANDLES) return 0;
    pthread_mutex_lock(&g_handle_mu);
    tm_object *o = g_handles[i];
    g_handles[i] = NULL;
    pthread_mutex_unlock(&g_handle_mu);
    if (!o) return 0;
    tm_sync_lock();
    tm_obj_release(o);
    tm_sync_unlock();
    return 1;
}

uint32_t tm_handle_dup(uint32_t h) {
    tm_object *o = tm_handle_get(h, 0);
    if (!o) return 0;
    return tm_handle_new(o);
}

/* ------------------------------------------------ strings */
size_t tm_wcslen(uint32_t wva) {
    size_t n = 0;
    if (!wva) return 0;
    while (rd16(wva + 2 * n)) n++;
    return n;
}

static int put_utf8(uint32_t cp, char *out, size_t cap, size_t *pos) {
    char tmp[4]; int k;
    if (cp < 0x80) { tmp[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*pos + k + 1 > cap) return 0;
    memcpy(out + *pos, tmp, k); *pos += k;
    return 1;
}

char *tm_w2u_n(uint32_t wva, int nchars, char *out, size_t cap) {
    size_t pos = 0;
    if (!cap) return out;
    if (!wva) { out[0] = 0; return out; }
    for (int i = 0; nchars < 0 || i < nchars; i++) {
        uint32_t c = rd16(wva + 2 * i);
        if (nchars < 0 && !c) break;
        if (c >= 0xD800 && c < 0xDC00) {
            uint32_t d = rd16(wva + 2 * (i + 1));
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
        }
        if (!put_utf8(c, out, cap, &pos)) break;
    }
    out[pos] = 0;
    return out;
}

char *tm_w2u(uint32_t wva, char *out, size_t cap) { return tm_w2u_n(wva, -1, out, cap); }

int tm_utf8_to_utf16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 0x3F); p += 1; }
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (c >> 10)); out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 >= cap) break;
            out[n++] = (uint16_t)c;
        }
    }
    if (cap) out[n] = 0;
    return n;
}

int tm_u2w(const char *s, uint32_t wva, int cap_chars) {
    uint16_t tmp[4096];
    int n = tm_utf8_to_utf16(s, tmp, cap_chars < 4096 ? cap_chars : 4096);
    memcpy(G2H(wva), tmp, (size_t)(n + 1) * 2);
    return n;
}
