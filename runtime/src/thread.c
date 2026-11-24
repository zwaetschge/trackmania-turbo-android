/*
 * thread.c - guest threads on host pthreads, TEB/PEB, TLS/FLS slots,
 * thread objects and the boot sequence.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

struct tm_thread {
    tm_object obj;            /* waitable: signaled on exit */
    uint32_t tid;
    uint32_t teb;
    uint32_t stack_lo, stack_hi;
    uint32_t start, param;
    uint32_t exit_code;       /* STILL_ACTIVE = 259 */
    int suspended;
    int priority;
    jmp_buf exit_jmp;
    pthread_t host;
};

#ifdef RECOMP_CPU_STRUCT
/* In the per-thread state object: no thread-local lookup on the hot paths
 * (critical sections, TLS slots, last error). Host threads that never ran guest
 * code have no state object and read as "no thread". */
#define t_self (*(tm_thread **)&RECOMP_CPU->c_host[0])
#define T_SELF_OK tm_cpu_bound()
#else
static __thread tm_thread *t_self;
#define T_SELF_OK 1
#endif
static uint32_t g_peb;
static uint32_t g_next_tid = 0x1004;
static pthread_mutex_t g_tid_mu = PTHREAD_MUTEX_INITIALIZER;
#define MAX_THREADS 1024
static tm_thread *g_threads[MAX_THREADS];

tm_thread *tm_thread_self(void) { return T_SELF_OK ? t_self : NULL; }
uint32_t tm_thread_teb(void) { return T_SELF_OK && t_self ? t_self->teb : 0; }
uint32_t tm_thread_id(void) { return T_SELF_OK && t_self ? t_self->tid : 0; }
uint32_t tm_peb(void) { return g_peb; }
uint32_t tm_get_last_error(void) { return T_SELF_OK && t_self ? rd32(t_self->teb + 0x34) : 0; }
void tm_set_last_error(uint32_t e) { if (T_SELF_OK && t_self) wr32(t_self->teb + 0x34, e); }

static void build_peb(void) {
    if (g_peb) return;
    g_peb = tm_galloc(0x1000);
    uint32_t p = g_peb;
    wr8(p + 2, 0);                         /* BeingDebugged */
    wr32(p + 8, g_image.image_base);       /* ImageBaseAddress */
    wr32(p + 0x18, tm_heap_process());     /* ProcessHeap */
    wr32(p + 0x64, 4);                     /* NumberOfProcessors */
    wr32(p + 0xA4, 6);                     /* OSMajorVersion: Windows 7 SP1 */
    wr32(p + 0xA8, 1);
    wr16(p + 0xAC, 7601);
    wr32(p + 0xB0, 2);                     /* VER_PLATFORM_WIN32_NT */
}

static tm_thread *thread_new(uint32_t stack_size) {
    tm_thread *t = calloc(1, sizeof *t);
    t->obj.kind = TMO_THREAD;
    t->obj.manual_reset = 1;
    t->exit_code = 259;
    pthread_mutex_lock(&g_tid_mu);
    t->tid = g_next_tid; g_next_tid += 4;
    for (int i = 0; i < MAX_THREADS; i++) if (!g_threads[i]) { g_threads[i] = t; break; }
    pthread_mutex_unlock(&g_tid_mu);
    if (stack_size < (1u << 20)) stack_size = 1u << 20;
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    t->stack_lo = tm_vm_alloc(0, stack_size, 0x3000, 4);
    if (!t->stack_lo) tm_fatal("guest stack allocation failed (%u bytes)", stack_size);
    t->stack_hi = t->stack_lo + stack_size;
    t->teb = tm_vm_alloc(0, 0x2000, 0x3000, 4);
    uint32_t teb = t->teb;
    wr32(teb + 0x00, 0xFFFFFFFFu);         /* SEH chain end */
    wr32(teb + 0x04, t->stack_hi);
    wr32(teb + 0x08, t->stack_lo);
    wr32(teb + 0x18, teb);                 /* Self */
    wr32(teb + 0x20, 0x1234);              /* pid */
    wr32(teb + 0x24, t->tid);
    wr32(teb + 0x30, g_peb);
    tm_tls_thread_init(teb);
    return t;
}

static void bind_thread(tm_thread *t) {
    t_self = t;
    g_fs_base = t->teb;
    g_esp = t->stack_hi - 64;
    g_ebp = 0;
    g_fpu_cw = 0x027F;
    g_mxcsr = 0x1F80;
}

int tm_thread_attach_main(uint32_t stack_size) {
    build_peb();
    tm_thread *t = thread_new(stack_size);
    t->host = pthread_self();
    bind_thread(t);
    return 0;
}

uint32_t tm_thread_lookup_tid(uint32_t tid, tm_thread **out) {
    pthread_mutex_lock(&g_tid_mu);
    for (int i = 0; i < MAX_THREADS; i++) if (g_threads[i] && g_threads[i]->tid == tid) { *out = g_threads[i]; pthread_mutex_unlock(&g_tid_mu); return 1; }
    pthread_mutex_unlock(&g_tid_mu);
    return 0;
}

static void thread_finish(tm_thread *t, uint32_t code) {
    tm_sync_lock();
    t->exit_code = code;
    t->obj.signaled = 1;
    tm_sync_wake(&t->obj);
    tm_sync_unlock();
}

static void *thread_main(void *arg) {
    tm_thread *t = arg;
    tm_machine_lock();
    bind_thread(t);
    /* wait while suspended (ResumeThread clears it) */
    while (t->suspended) {
        tm_machine_unlock();
        tm_sync_lock();
        while (t->suspended) tm_sync_wait(0xFFFFFFFFu, 0);
        tm_sync_unlock();
        tm_machine_lock();
    }
    uint32_t code;
    if (setjmp(t->exit_jmp) == 0) {
        tm_tls_callbacks(2);   /* DLL_THREAD_ATTACH */
        TM_INFO("guest thread %u starts at %08X(%08X)", t->tid, t->start, t->param);
        code = tm_call_guest(t->start, 1, t->param);
    } else {
        code = t->exit_code == 259 ? 0 : t->exit_code;
    }
    TM_INFO("guest thread %u exited with %u", t->tid, code);
    thread_finish(t, code);
    tm_machine_unlock();
    return NULL;
}

/* CreateThread core; returns handle, writes tid. */
uint32_t tm_create_thread(uint32_t stack_size, uint32_t start, uint32_t param, uint32_t flags, uint32_t *tid_out) {
    tm_thread *t = thread_new(stack_size);
    t->start = start; t->param = param;
    t->suspended = (flags & 4) ? 1 : 0;
    uint32_t h = tm_handle_new(&t->obj);
    t->obj.refs++;  /* the running thread keeps itself alive */
    if (tid_out) *tid_out = t->tid;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 32u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->host, &a, thread_main, t) != 0) tm_fatal("pthread_create failed");
    pthread_attr_destroy(&a);
    return h;
}

void tm_exit_thread(uint32_t code) {
    tm_thread *t = t_self;
    if (!t || !t->start) {
        TM_INFO("ExitThread on the main thread -> process exit %u", code);
        g_guest_exit_code = (int)code;
        g_guest_exit_requested = 1;
        tm_machine_unlock();
        _exit((int)code);
    }
    t->exit_code = code;
    longjmp(t->exit_jmp, 1);
}

int tm_resume_thread(uint32_t h, uint32_t *prev) {
    tm_object *o = tm_handle_get(h, TMO_THREAD);
    if (!o) return 0;
    tm_thread *t = (tm_thread *)o;
    tm_sync_lock();
    *prev = (uint32_t)t->suspended;
    if (t->suspended) t->suspended--;
    tm_sync_broadcast();
    tm_sync_unlock();
    return 1;
}

uint32_t tm_thread_exit_code(uint32_t h, int *ok) {
    if (h == TM_PSEUDO_THREAD) { *ok = 1; return 259; }
    tm_object *o = tm_handle_get(h, TMO_THREAD);
    if (!o) { *ok = 0; return 0; }
    *ok = 1;
    return ((tm_thread *)o)->exit_code;
}

uint32_t tm_thread_handle_tid(uint32_t h) {
    if (h == TM_PSEUDO_THREAD) return tm_thread_id();
    tm_object *o = tm_handle_get(h, TMO_THREAD);
    return o ? ((tm_thread *)o)->tid : 0;
}

int *tm_thread_priority_ptr(uint32_t h) {
    if (h == TM_PSEUDO_THREAD) return T_SELF_OK && t_self ? &t_self->priority : NULL;
    tm_object *o = tm_handle_get(h, TMO_THREAD);
    return o ? &((tm_thread *)o)->priority : NULL;
}

/* ------------------------------------------------ TLS / FLS slots in the TEB */
static uint8_t g_tls_used[1088];
static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;

uint32_t tm_tls_alloc(void) {
    pthread_mutex_lock(&g_tls_mu);
    for (uint32_t i = 0; i < 64; i++) if (!g_tls_used[i]) { g_tls_used[i] = 1; pthread_mutex_unlock(&g_tls_mu); return i; }
    pthread_mutex_unlock(&g_tls_mu);
    return 0xFFFFFFFFu;
}
int tm_tls_free(uint32_t i) {
    if (i >= 64) return 0;
    pthread_mutex_lock(&g_tls_mu);
    g_tls_used[i] = 0;
    pthread_mutex_unlock(&g_tls_mu);
    /* clear the slot in every live thread */
    for (int k = 0; k < MAX_THREADS; k++) if (g_threads[k]) wr32(g_threads[k]->teb + 0xE10 + 4 * i, 0);
    return 1;
}
uint32_t tm_tls_get(uint32_t i) { return i < 64 && T_SELF_OK && t_self ? rd32(t_self->teb + 0xE10 + 4 * i) : 0; }
int tm_tls_set(uint32_t i, uint32_t v) { if (i >= 64 || !T_SELF_OK || !t_self) return 0; wr32(t_self->teb + 0xE10 + 4 * i, v); return 1; }

/* ------------------------------------------------ boot */
/* TM_WATCHDOG=first[,every] (seconds): dump every live guest thread's state at
 * that time - where a run that looks stuck actually is. Each thread prints its
 * own registers and call trail from a signal handler (they are thread-local). */
#include <signal.h>
static void watchdog_sig(int sig) { (void)sig; tm_dump_state("watchdog"); }
static void *watchdog_main(void *arg) {
    int first = ((int *)arg)[0], every = ((int *)arg)[1];
    sleep((unsigned)first);
    for (;;) {
        TM_ERR("==== watchdog: state of all guest threads ====");
        pthread_mutex_lock(&g_tid_mu);
        for (int i = 0; i < MAX_THREADS; i++)
            if (g_threads[i] && g_threads[i]->exit_code == 259 && g_threads[i]->host) pthread_kill(g_threads[i]->host, SIGUSR2);
        pthread_mutex_unlock(&g_tid_mu);
        if (every <= 0) break;
        sleep((unsigned)every);
    }
    return NULL;
}
static void watchdog_start(void) {
    static int cfg[2];
    const char *e = getenv("TM_WATCHDOG");
    if (!e || sscanf(e, "%d,%d", &cfg[0], &cfg[1]) < 1 || cfg[0] <= 0) return;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = watchdog_sig; sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
    pthread_t t; pthread_create(&t, NULL, watchdog_main, cfg); pthread_detach(t);
}

int tm_run(const char *exe_path) {
    if (tm_mem_init() != 0) return 90;
    tm_shims_register_all();
    if (tm_load_image(exe_path) != 0) return 91;
    tm_bind_imports();
    tm_machine_lock();
    tm_thread_attach_main(16u << 20);
    watchdog_start();
    tm_status("Image mapped, %u lifted functions available", recomp_dispatch_count);
    tm_tracks_import();       /* community tracks dropped into <user>/ImportTracks (tracks.c) */
    tm_video_register();      /* native VP8 decoder in place of the game's (video.c) */
    tm_tweaks_register();     /* optional deviations from the original (tweaks.c) */
    tm_lan_register();        /* LAN host/join (lan.c) */
    tm_guards_register();     /* answers for reads through NULL objects (guards.c) */
    tm_parallel_register();   /* functions that run outside the machine lock */
    tm_load_lifted_modules(); /* bundled DLLs recompiled as extra modules */
    tm_tls_callbacks(1);      /* DLL_PROCESS_ATTACH */
    tm_status("Running original entry point %08X", g_image.entry);
    uint32_t r = tm_call_guest(g_image.entry, 0);
    TM_INFO("entry returned %u", r);
    tm_machine_unlock();
    return (int)r;
}
