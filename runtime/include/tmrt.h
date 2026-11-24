/*
 * tmrt.h - portable host runtime for the statically recompiled
 * TrackmaniaTurbo.exe (PE32 x86) on 64-bit hosts (Android arm64, Linux x86-64).
 *
 * Guest model
 *   - Guest virtual addresses are 32-bit and preserved (image at 0x00400000).
 *     Host address = g_mem_base + (uint32_t)va. Never cast a guest pointer to a
 *     host pointer directly; use G2H().
 *   - Every guest thread runs on its own host pthread. The guest register file
 *     is thread-local (RECOMP_TLS = __thread). Lifted code only runs while the
 *     thread holds the machine lock, which serialises guest execution and keeps
 *     x86 memory-ordering assumptions valid on weakly ordered ARM64.
 *   - Imports are host "shims" with the pcrecomp shim ABI: void f(void),
 *     entered with g_esp -> [ret][arg0][arg1]...; the shim pops ret + args
 *     (stdcall) or only ret (cdecl) and leaves its result in g_eax.
 */
#ifndef TMRT_H
#define TMRT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "recomp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- memory */
#define G2H(va) ((void *)((uintptr_t)(uint32_t)(va) + (uintptr_t)g_mem_base))
static inline uint32_t H2G(const void *p) { return (uint32_t)((uintptr_t)p - (uintptr_t)g_mem_base); }

static inline uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, G2H(va), 4); return v; }
static inline uint16_t rd16(uint32_t va) { uint16_t v; memcpy(&v, G2H(va), 2); return v; }
static inline uint8_t  rd8(uint32_t va)  { return *(uint8_t *)G2H(va); }
static inline void wr32(uint32_t va, uint32_t v) { memcpy(G2H(va), &v, 4); }
static inline void wr16(uint32_t va, uint16_t v) { memcpy(G2H(va), &v, 2); }
static inline void wr8(uint32_t va, uint8_t v)   { *(uint8_t *)G2H(va) = v; }

/* Guest address-space layout (all below 4 GB). */
#define TM_NULL_GUARD      0x00010000u   /* [0, 64K) never committed            */
#define TM_ALLOC_LO        0x02000000u   /* VirtualAlloc / heap arena start      */
#define TM_ALLOC_HI        0xF0000000u   /* arena end                            */
#define TM_SHIM_BASE       0xFFE00000u   /* synthetic import addresses           */
#define TM_SHIM_STRIDE     16u
#define TM_SHIM_MAX        0x10000u
#define TM_HMOD_BASE       0x7F000000u   /* synthetic HMODULEs of shim modules   */

void     tm_add_override(uint32_t va, recomp_func_t fn);   /* host replacement for one guest function (indirect calls) */
void     tm_free_region(recomp_func_t fn);        /* run fn outside the machine lock (see parallel.c) */
int      tm_diag_on(void);                         /* TM_FRAME_LOG=<ms>: long-frame diagnostics */
void     tm_diag_add(int kind, uint64_t ns);       /* 0 machine wait, 1 file reads (per thread) */
uint64_t tm_diag_take(int kind);
void     tm_input_mouse(float nx, float ny, int buttons, int wheel);   /* input.c: 0..1 over the surface */
void     tm_input_mouse_state(uint8_t *out, uint32_t cb);
int      tm_input_mouse_events(uint32_t *ofs, uint32_t *data, uint32_t *time, uint32_t *seq, int max, int remove);
void     tm_tweaks_register(void);                 /* tweaks.c */
void     tm_tracks_import(void);                   /* tracks.c: <user>/ImportTracks -> the game's track folder */
void     tm_video_register(void);                  /* video.c: native VP8 decoding */
int      tm_video_native(void);
void     tm_parallel_register(void);
recomp_func_t tm_parallel_wrap_export(const char *dll, const char *name, recomp_func_t fn);
void     tm_cpu_bind(void);                        /* give this host thread a guest state object */
#if defined(RECOMP_CPU_STRUCT) && defined(RECOMP_CPU_TLS_SLOT)
static inline int tm_cpu_bound(void) { return recomp_cpu_get() != NULL; }
#else
static inline int tm_cpu_bound(void) { return 1; }
#endif
void     tm_memdump_request(void);                 /* diagnostics: dump guest memory (F11) */
int      tm_mem_init(void);                        /* reserve 4 GB + guard   */
int      tm_mem_commit(uint32_t va, uint32_t size);/* make RW (host pages)   */
uint32_t tm_vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot);
int      tm_vm_free(uint32_t addr, uint32_t size, uint32_t type);
int      tm_vm_query(uint32_t addr, uint32_t out_mbi);   /* MEMORY_BASIC_INFORMATION */
int      tm_vm_protect(uint32_t addr, uint32_t size, uint32_t prot, uint32_t *old);
void     tm_vm_mark_image(uint32_t base, uint32_t size);

/* Guest heaps (HeapCreate/HeapAlloc/...). Handle 0 = invalid. */
uint32_t tm_heap_create(uint32_t flags);
uint32_t tm_heap_process(void);
uint32_t tm_heap_alloc(uint32_t heap, uint32_t flags, uint32_t size);
uint32_t tm_heap_realloc(uint32_t heap, uint32_t flags, uint32_t ptr, uint32_t size);
int      tm_heap_free(uint32_t heap, uint32_t flags, uint32_t ptr);
uint32_t tm_heap_size(uint32_t heap, uint32_t flags, uint32_t ptr);
int      tm_heap_destroy(uint32_t heap);
/* convenience for host-side allocations that the guest may see */
uint32_t tm_galloc(uint32_t size);
void     tm_gfree(uint32_t va);
uint32_t tm_gstrdup(const char *s);
uint32_t tm_gwstrdup_utf8(const char *s);

/* ---------------------------------------------------------------- logging */
enum { TM_LOG_ERR = 0, TM_LOG_WARN = 1, TM_LOG_INFO = 2, TM_LOG_TRACE = 3 };
extern int tm_log_level;
void tm_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define TM_ERR(...)   tm_log(TM_LOG_ERR, __VA_ARGS__)
#define TM_WARN(...)  tm_log(TM_LOG_WARN, __VA_ARGS__)
#define TM_INFO(...)  tm_log(TM_LOG_INFO, __VA_ARGS__)
#define TM_TRACE(...) do { if (tm_log_level >= TM_LOG_TRACE) tm_log(TM_LOG_TRACE, __VA_ARGS__); } while (0)
/* Stops the process with a full diagnostic (guest registers, call rings). */
void tm_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void tm_dump_state(const char *why);
/* Host hook for the platform layer (Android: logcat + on-screen status). */
extern void (*tm_log_sink)(int level, const char *line);
extern void (*tm_status_sink)(const char *line);
extern void (*tm_fatal_hook)(const char *why);   /* may block forever */
void tm_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------------------------------------------------------------- shim ABI */
#define ARG(n)      rd32(g_esp + 4u + 4u * (uint32_t)(n))
#define ARGP(n)     ((uint32_t)ARG(n))
#define ARGS(n)     ((const char *)(ARG(n) ? G2H(ARG(n)) : NULL))
#define ARGF(n)     ({ uint32_t _u = ARG(n); float _f; memcpy(&_f, &_u, 4); _f; })
/* stdcall: callee pops return address + nargs dwords. */
#define RET(val, nargs) do { g_eax = (uint32_t)(val); g_esp += 4u + 4u * (uint32_t)(nargs); return; } while (0)
/* 64-bit result in edx:eax */
#define RET64(val, nargs) do { uint64_t _v = (uint64_t)(val); g_eax = (uint32_t)_v; g_edx = (uint32_t)(_v >> 32); g_esp += 4u + 4u * (uint32_t)(nargs); return; } while (0)
/* cdecl: caller pops arguments. */
#define RETC(val)   do { g_eax = (uint32_t)(val); g_esp += 4u; return; } while (0)
/* float result for cdecl functions returning float/double on the x87 stack */
#define RETC_ST0(dval) do { fp_push_impl(g_st, &g_fp_top, (double)(dval)); g_esp += 4u; return; } while (0)

typedef void (*tm_shim_fn)(void);
typedef struct {
    const char *dll;     /* lower-case, e.g. "kernel32.dll" */
    const char *name;    /* export name */
    tm_shim_fn  fn;
} tm_shim_def;

void tm_register_shims(const tm_shim_def *defs, size_t count);
/* Synthetic guest VA for dll!name; creates a strict "missing" stub if absent. */
uint32_t tm_import_va(const char *dll, const char *name, int create_missing);
/* GetProcAddress semantics: 0 when not implemented. */
uint32_t tm_proc_address(const char *dll, const char *name);
const char *tm_import_name(uint32_t va);   /* "dll!name" or NULL */
uint32_t tm_module_handle(const char *dll); /* 0 if no shim module of that name */
const char *tm_module_name(uint32_t hmod);
int tm_module_known(const char *dll);

#define TM_SHIM(dll, name) { dll, #name, shim_##name }

/* ---------------------------------------------------------------- network (net.c, lan.c) */
int  tm_net_lan(void);                 /* TM_NET_LAN=1: the local network is reachable */
int  tm_net_local_ip(uint32_t *addr_be, uint32_t *mask_be, char *ifname, size_t ifname_len);
void tm_lan_register(void);            /* LAN host/join helpers (TM_LAN) */
void tm_guards_register(void);         /* guards.c */
/* hook sites (config/hooks.txt): fn(va) runs before the instruction at va */
void tm_hook_register(uint32_t va, void (*fn)(uint32_t));
/* C++ exceptions (eh.c) */
void tm_eh_wrap(uint32_t va, void (*body)(void));
void tm_eh_raise(uint32_t obj, uint32_t throwinfo, uint32_t from);

/* ---------------------------------------------------------------- guest calls */
/* Call guest code at va with dword args (pushed right to left). Works for
 * cdecl and stdcall (esp is restored from a saved copy). Returns eax. */
uint32_t tm_call_guest(uint32_t va, int nargs, ...);
uint32_t tm_call_guest_v(uint32_t va, int nargs, const uint32_t *args);
/* thiscall: ecx = this */
uint32_t tm_call_guest_this(uint32_t va, uint32_t self, int nargs, ...);

/* ---------------------------------------------------------------- machine lock */
void tm_machine_lock(void);
void tm_machine_unlock(void);
/* Release the machine around a blocking host operation. */
#define TM_BLOCKING_BEGIN() tm_machine_unlock()
#define TM_BLOCKING_END()   tm_machine_lock()

/* ---------------------------------------------------------------- threads/TEB */
typedef struct tm_thread tm_thread;
tm_thread *tm_thread_self(void);
uint32_t   tm_thread_teb(void);
uint32_t   tm_thread_id(void);
uint32_t   tm_get_last_error(void);
void       tm_set_last_error(uint32_t e);
/* Create the TEB, stack and static TLS for the calling host thread. */
int        tm_thread_attach_main(uint32_t stack_size);
uint32_t   tm_peb(void);

/* ---------------------------------------------------------------- image */
typedef struct {
    uint32_t image_base, size_of_image, entry;
    uint32_t tls_dir;            /* VA of IMAGE_TLS_DIRECTORY32 or 0 */
    uint32_t tls_index_va;       /* AddressOfIndex */
    uint32_t tls_tmpl_start, tls_tmpl_end, tls_zero_fill;
    uint32_t tls_callbacks;      /* VA of null-terminated callback array */
    uint32_t rsrc_va, rsrc_size;
    uint32_t iat_lo, iat_hi;     /* IAT range (for lookup of `call [slot]`) */
    char     path[1024];         /* host path of the EXE */
    char     dir[1024];          /* host directory of the EXE */
} tm_image;
extern tm_image g_image;
int tm_load_image(const char *host_path);
int tm_bind_imports(void);

/* Bundled DLLs lifted as extra modules (tools/lift_dll.py). Each is linked in
 * with its own dispatch table and mapped from the user's game directory at the
 * fixed base it was lifted for; its exports then satisfy the main image's
 * imports of that DLL. */
typedef struct {
    const char *dll;                              /* file name in the game directory */
    uint32_t base;                                /* base the module was lifted at */
    const recomp_dispatch_entry_t *table;         /* NULL when the module is not linked in */
    const uint32_t *count;
} tm_lifted_module;
extern const tm_lifted_module tm_lifted_modules[];
void tm_load_lifted_modules(void);                /* map, bind, register exports, run DllMain */
/* Allocate and initialise this thread's static TLS block (fs:[0x2C]). */
int tm_tls_thread_init(uint32_t teb);
void tm_tls_callbacks(uint32_t reason);

/* ---------------------------------------------------------------- handles */
enum tm_obj_kind {
    TMO_NONE = 0, TMO_EVENT, TMO_MUTEX, TMO_SEMAPHORE, TMO_THREAD, TMO_FILE,
    TMO_FIND, TMO_MAPPING, TMO_PROCESS, TMO_HEAP_DUMMY, TMO_MODULE, TMO_KEY,
    TMO_TIMER, TMO_SOCKET, TMO_INET, TMO_MISC
};
typedef struct tm_object {
    int kind;
    int refs;
    void (*destroy)(struct tm_object *);
    /* waitable state (protected by the sync mutex) */
    int signaled;
    int manual_reset;
    int count, max_count;        /* semaphore */
    uint32_t owner_tid; int recursion; /* mutex */
    void *impl;                  /* kind-specific */
} tm_object;
uint32_t   tm_handle_new(tm_object *o);
tm_object *tm_handle_get(uint32_t h, int kind);   /* kind 0 = any */
int        tm_handle_close(uint32_t h);
uint32_t   tm_handle_dup(uint32_t h);
#define TM_PSEUDO_PROCESS 0xFFFFFFFFu
#define TM_PSEUDO_THREAD  0xFFFFFFFEu

/* sync: one mutex + condvar for all waitable objects */
void tm_sync_lock(void);
void tm_sync_unlock(void);
void tm_sync_broadcast(void);
void tm_sync_wake(const void *key);                 /* wake waiters of one object (NULL: all) */
int  tm_sync_wait_keys(const void *const *keys, int nkeys, uint32_t ms_deadline_rel, uint64_t start_ns);
/* wait on cv with ms timeout (0xFFFFFFFF = infinite); returns 0 on timeout */
int  tm_sync_wait(uint32_t ms_deadline_rel, uint64_t start_ns);
uint64_t tm_now_ns(void);
uint32_t tm_guest_tick_ms(void);                  /* the guest's GetTickCount clock (message and input timestamps) */

/* input.c: platform input state (fed by the platform layer). */
void tm_input_init(void);
void tm_input_pad(int connected, uint32_t buttons, int lt, int rt, int lx, int ly, int rx, int ry);
void tm_input_key(int vk, int down);
int  tm_input_pad_get(uint8_t *xinput_state16);
void tm_input_pad_slot(int pad, int connected, uint32_t buttons, int lt, int rt, int lx, int ly, int rx, int ry);
int  tm_input_pad_get_slot(int pad, uint8_t *xinput_state16);
void tm_input_rumble(int pad, int low, int high);          /* XInputSetState motor speeds 0..65535 */
extern void (*tm_rumble_sink)(int pad, int low, int high);  /* set by the platform layer */
void tm_input_kbd_state(uint8_t *out, uint32_t cb);
int  tm_input_kbd_events(uint32_t *dik, uint32_t *data, uint32_t *time, uint32_t *seq, int max, int remove);

/* ---------------------------------------------------------------- strings */
size_t tm_wcslen(uint32_t wva);
/* UTF-16 guest string -> UTF-8 host buffer (always NUL terminated). */
char  *tm_w2u(uint32_t wva, char *out, size_t cap);
char  *tm_w2u_n(uint32_t wva, int nchars, char *out, size_t cap);
/* UTF-8 -> UTF-16 into guest buffer; returns chars written excl. NUL. */
int    tm_u2w(const char *s, uint32_t wva, int cap_chars);
int    tm_utf8_to_utf16(const char *s, uint16_t *out, int cap);

/* ---------------------------------------------------------------- paths/files */
/* Map a Windows path (ANSI/UTF-8) to a host path. Returns 0 on success. */
int  tm_path_to_host(const char *winpath, char *out, size_t cap);
void tm_path_set_roots(const char *game_dir, const char *user_dir, const char *temp_dir);
extern char g_game_dir[1024], g_user_dir[1024], g_temp_dir[1024];
extern const char *g_win_game_dir;   /* "C:\\TrackmaniaTurbo" */

/* ---------------------------------------------------------------- init */
void tm_shims_register_all(void);
/* Full boot: load image, bind imports, run TLS callbacks + entry. Does not
 * return unless the guest exits; returns the exit code. */
int  tm_run(const char *exe_path);
extern volatile int g_guest_exit_requested;
extern int g_guest_exit_code;

/* Platform hooks set by the host layer (desktop / Android). */
typedef struct {
    void (*on_window_created)(uint32_t hwnd, int w, int h);
    int  (*get_display_size)(int *w, int *h);
} tm_platform_hooks;
extern tm_platform_hooks g_platform;

#ifdef __cplusplus
}
#endif
#endif
