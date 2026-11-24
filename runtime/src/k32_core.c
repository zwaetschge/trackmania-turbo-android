/*
 * k32_core.c - kernel32: process, modules, memory, heaps, TLS/FLS, errors,
 * time, system info, sync objects, threads, interlocked, debug.
 * Every shim pops exactly the stdcall argument bytes of the real API.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sched.h>
#include <ctype.h>

uint32_t tm_create_thread(uint32_t stack_size, uint32_t start, uint32_t param, uint32_t flags, uint32_t *tid_out);
void tm_exit_thread(uint32_t code);
int tm_resume_thread(uint32_t h, uint32_t *prev);
uint32_t tm_thread_exit_code(uint32_t h, int *ok);
uint32_t tm_thread_handle_tid(uint32_t h);
int *tm_thread_priority_ptr(uint32_t h);
uint32_t tm_tls_alloc(void);
int tm_tls_free(uint32_t i);
uint32_t tm_tls_get(uint32_t i);
int tm_tls_set(uint32_t i, uint32_t v);

#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NOT_SUPPORTED 50
#define ERROR_CALL_NOT_IMPLEMENTED 120

/* ------------------------------------------------------------ process/module */
static uint32_t g_cmdline_a, g_cmdline_w;
static const char *g_cmdline = "\"C:\\TrackmaniaTurbo\\TrackmaniaTurbo.exe\"";
const char *g_win_game_dir = "C:\\TrackmaniaTurbo";

static void ensure_cmdline(void) {
    if (!g_cmdline_a) {
        const char *extra = getenv("TM_CMDLINE");
        char buf[1024];
        snprintf(buf, sizeof buf, "%s%s%s", g_cmdline, extra ? " " : "", extra ? extra : "");
        g_cmdline_a = tm_gstrdup(buf);
        g_cmdline_w = tm_gwstrdup_utf8(buf);
    }
}
static void shim_GetCommandLineA(void) { ensure_cmdline(); RET(g_cmdline_a, 0); }
static void shim_GetCommandLineW(void) { ensure_cmdline(); RET(g_cmdline_w, 0); }

static uint32_t hmodule_for(const char *name) {
    if (!name) return g_image.image_base;
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    if (!strcasecmp(base, "TrackmaniaTurbo.exe") || !strcasecmp(base, "TrackmaniaTurbo")) return g_image.image_base;
    return tm_module_handle(base);
}

static void shim_GetModuleHandleA(void) {
    uint32_t h = hmodule_for(ARGS(0));
    if (!h) tm_set_last_error(ERROR_MOD_NOT_FOUND);
    RET(h, 1);
}
static void shim_GetModuleHandleW(void) {
    char n[512];
    uint32_t h = ARG(0) ? hmodule_for(tm_w2u(ARG(0), n, sizeof n)) : g_image.image_base;
    if (!h) tm_set_last_error(ERROR_MOD_NOT_FOUND);
    RET(h, 1);
}
static void shim_GetModuleHandleExW(void) {
    uint32_t flags = ARG(0), name = ARG(1), out = ARG(2);
    uint32_t h;
    if (flags & 4) { /* FROM_ADDRESS */
        h = (name >= g_image.image_base && name < g_image.image_base + g_image.size_of_image) ? g_image.image_base : 0;
    } else {
        char n[512];
        h = name ? hmodule_for(tm_w2u(name, n, sizeof n)) : g_image.image_base;
    }
    if (out) wr32(out, h);
    if (!h) tm_set_last_error(ERROR_MOD_NOT_FOUND);
    RET(h != 0, 3);
}

static const char *exe_win_path(void) { return "C:\\TrackmaniaTurbo\\TrackmaniaTurbo.exe"; }

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2);
    const char *p = (!h || h == g_image.image_base) ? exe_win_path() : tm_module_name(h);
    if (!p) { tm_set_last_error(ERROR_MOD_NOT_FOUND); RET(0, 3); }
    uint32_t len = (uint32_t)strlen(p);
    if (!n) RET(0, 3);
    uint32_t c = len < n ? len : n - 1;
    memcpy(G2H(buf), p, c); wr8(buf + c, 0);
    if (len >= n) tm_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    RET(len >= n ? n : len, 3);
}
static void shim_GetModuleFileNameW(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2);
    const char *p = (!h || h == g_image.image_base) ? exe_win_path() : tm_module_name(h);
    if (!p) { tm_set_last_error(ERROR_MOD_NOT_FOUND); RET(0, 3); }
    uint32_t len = (uint32_t)strlen(p);
    if (!n) RET(0, 3);
    uint16_t tmp[1024]; int w = tm_utf8_to_utf16(p, tmp, 1024);
    uint32_t c = (uint32_t)w < n ? (uint32_t)w : n - 1;
    memcpy(G2H(buf), tmp, c * 2); wr16(buf + 2 * c, 0);
    if (len >= n) tm_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    RET((uint32_t)w >= n ? n : (uint32_t)w, 3);
}

/* LoadLibrary: only modules we implement exist. api-ms-win-* sets resolve to
 * kernel32 when we have shims for them (GetProcAddress decides per name). */
static uint32_t load_library(const char *name) {
    if (!name) return 0;
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    char low[128]; size_t i;
    for (i = 0; base[i] && i < sizeof low - 5; i++) low[i] = (char)tolower((unsigned char)base[i]);
    low[i] = 0;
    if (!strncmp(low, "api-ms-win-core-", 16) || !strncmp(low, "ext-ms-win-", 11)) return tm_module_handle("kernel32.dll");
    uint32_t h = hmodule_for(base);
    TM_INFO("LoadLibrary(%s) -> %08X", name, h);
    if (!h) tm_set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}
static void shim_LoadLibraryA(void) { RET(load_library(ARGS(0)), 1); }
static void shim_LoadLibraryW(void) { char n[512]; RET(load_library(tm_w2u(ARG(0), n, sizeof n)), 1); }
static void shim_LoadLibraryExW(void) { char n[512]; RET(load_library(tm_w2u(ARG(0), n, sizeof n)), 3); }
static void shim_LoadLibraryExA(void) { RET(load_library(ARGS(0)), 3); }
static void shim_FreeLibrary(void) { RET(1, 1); }

static void shim_GetProcAddress(void) {
    uint32_t h = ARG(0), name = ARG(1);
    char nm[256];
    if (name < 0x10000) snprintf(nm, sizeof nm, "#%u", name);
    else snprintf(nm, sizeof nm, "%s", (const char *)G2H(name));
    const char *dll = h == g_image.image_base ? NULL : tm_module_name(h);
    uint32_t r = dll ? tm_proc_address(dll, nm) : 0;
    TM_INFO("GetProcAddress(%s, %s) -> %08X", dll ? dll : "?", nm, r);
    if (!r) tm_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(r, 2);
}

static void shim_GetCurrentProcess(void) { RET(TM_PSEUDO_PROCESS, 0); }
static void shim_GetCurrentProcessId(void) { RET(0x1234, 0); }
static void shim_GetCurrentThread(void) { RET(TM_PSEUDO_THREAD, 0); }
static void shim_GetCurrentThreadId(void) { RET(tm_thread_id(), 0); }

static void shim_ExitProcess(void) {
    uint32_t code = ARG(0);
    TM_INFO("ExitProcess(%u) from %08X", code, g_cur_func);
    tm_status("Game called ExitProcess(%u)", code);
    g_guest_exit_code = (int)code;
    g_guest_exit_requested = 1;
    fflush(stdout); fflush(stderr);
    { char why[64]; snprintf(why, sizeof why, "ExitProcess(%u)", code); if (tm_fatal_hook) tm_fatal_hook(why); }
    _exit((int)code);
}
static void shim_TerminateProcess(void) {
    TM_INFO("TerminateProcess(%08X, %u) from %08X", ARG(0), ARG(1), g_cur_func);
    if (ARG(0) == TM_PSEUDO_PROCESS) { tm_status("Game called TerminateProcess(%u)", ARG(1)); _exit((int)ARG(1)); }
    RET(0, 2);
}

/* GetStartupInfoW: STARTUPINFOW is 68 bytes */
static void shim_SetHandleCount(void) { RET(ARG(0), 1); }             /* legacy CRT start-up: no-op */
static void shim_EnumSystemLocalesA(void) { RET(1, 2); }               /* no locales enumerated */
static void shim_GetStartupInfoW(void) { uint32_t p = ARG(0); memset(G2H(p), 0, 68); wr32(p, 68); RET(0, 1); }
static void shim_GetStartupInfoA(void) { uint32_t p = ARG(0); memset(G2H(p), 0, 68); wr32(p, 68); RET(0, 1); }

/* ------------------------------------------------------------ environment */
static void shim_GetEnvironmentStringsW(void) {
    static uint32_t env;
    if (!env) { env = tm_galloc(64); /* empty block: two NULs */ }
    RET(env, 0);
}
static void shim_FreeEnvironmentStringsW(void) { RET(1, 1); }
static void shim_SetEnvironmentVariableA(void) { RET(1, 2); }
static void shim_ExpandEnvironmentStringsA(void) {
    const char *s = ARGS(0); uint32_t dst = ARG(1), n = ARG(2);
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (dst && n >= len) memcpy(G2H(dst), s, len);
    RET(len, 3);
}

/* ------------------------------------------------------------ errors */
static void shim_GetLastError(void) { RET(tm_get_last_error(), 0); }
static void shim_SetLastError(void) { tm_set_last_error(ARG(0)); RET(0, 1); }
static void shim_SetErrorMode(void) { RET(0, 1); }
static void shim_UnhandledExceptionFilter(void) { RET(0, 1); }
static void shim_SetUnhandledExceptionFilter(void) { RET(0, 1); }
static void shim_IsDebuggerPresent(void) { RET(0, 0); }
static void shim_DebugBreak(void) { TM_WARN("DebugBreak from %08X", g_cur_func); RET(0, 0); }
static void shim_OutputDebugStringA(void) { TM_INFO("ODS: %s", ARGS(0) ? ARGS(0) : ""); RET(0, 1); }
static void shim_OutputDebugStringW(void) { char b[2048]; TM_INFO("ODS: %s", tm_w2u(ARG(0), b, sizeof b)); RET(0, 1); }
static void shim_EncodePointer(void) { RET(ARG(0), 1); }
static void shim_DecodePointer(void) { RET(ARG(0), 1); }
static void shim_RaiseException(void) {
    if (ARG(0) == 0x406D1388u && ARG(2) >= 3) {
        /* MSVC SetThreadName: raised inside __try/__except(EXECUTE_HANDLER)
         * purely for a debugger. Without one the handler swallows it, which is
         * exactly a return from here. */
        uint32_t info = ARG(3);
        if (rd32(info) == 0x1000) TM_INFO("thread %u named '%s'", rd32(info + 8) == 0xFFFFFFFFu ? tm_thread_id() : rd32(info + 8),
                                          rd32(info + 4) ? (const char *)G2H(rd32(info + 4)) : "");
        RET(0, 4);
    }
    if (ARG(0) == 0xE06D7363u && ARG(2) >= 3 && ARG(3)) {
        /* MSVC C++ throw: { magic, object, ThrowInfo }; eh.c finds the catch and
         * longjmps there; it returns only when nothing catches it. */
        uint32_t args = ARG(3), magic = rd32(args);
        if (magic == 0x19930520u || magic == 0x19930521u || magic == 0x19930522u)
            tm_eh_raise(rd32(args + 4), rd32(args + 8), g_cur_func);
    }
    tm_fatal("RaiseException(code %08X flags %u nargs %u args %08X) from %08X - no handler (guest SEH)",
             ARG(0), ARG(1), ARG(2), ARG(3), g_cur_func);
}
static void shim_RtlUnwind(void) { tm_fatal("RtlUnwind from %08X - guest SEH unwinding not implemented", g_cur_func); }
static void shim_RtlCaptureStackBackTrace(void) { RET(0, 4); }
static void shim_IsBadReadPtr(void) { RET(ARG(0) == 0, 2); }
static void shim_IsBadWritePtr(void) { RET(ARG(0) == 0, 2); }
static void shim_IsBadCodePtr(void) { RET(ARG(0) == 0, 1); }
static void shim_FormatMessageA(void) {
    uint32_t buf = ARG(4), n = ARG(5), flags = ARG(0);
    char msg[64]; snprintf(msg, sizeof msg, "Error %u", ARG(2));
    if (flags & 0x100) { uint32_t g = tm_gstrdup(msg); wr32(buf, g); RET(strlen(msg), 7); }
    if (buf && n > strlen(msg)) { strcpy((char *)G2H(buf), msg); RET(strlen(msg), 7); }
    RET(0, 7);
}

/* ------------------------------------------------------------ memory */
static void shim_VirtualAlloc(void) {
    uint32_t r = tm_vm_alloc(ARG(0), ARG(1), ARG(2), ARG(3));
    TM_TRACE("VirtualAlloc(%08X, %X, %X, %X) = %08X", ARG(0), ARG(1), ARG(2), ARG(3), r);
    if (!r) tm_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(r, 4);
}
static void shim_VirtualFree(void) { RET(tm_vm_free(ARG(0), ARG(1), ARG(2)), 3); }
static void shim_VirtualQuery(void) { tm_vm_query(ARG(0), ARG(1)); RET(28, 3); }
static void shim_VirtualProtect(void) {
    uint32_t old = 4;
    int ok = tm_vm_protect(ARG(0), ARG(1), ARG(2), &old);
    if (ARG(3)) wr32(ARG(3), old);
    RET(ok, 4);
}
static void shim_GetProcessHeap(void) { RET(tm_heap_process(), 0); }
static void shim_HeapCreate(void) { RET(tm_heap_create(ARG(0)), 3); }
static void shim_HeapDestroy(void) { RET(tm_heap_destroy(ARG(0)), 1); }
static void shim_HeapAlloc(void) {
    uint32_t r = tm_heap_alloc(ARG(0), ARG(1), ARG(2));
    if (!r) TM_WARN("HeapAlloc(%u) failed", ARG(2));
    RET(r, 3);
}
static void shim_HeapReAlloc(void) { RET(tm_heap_realloc(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
static void shim_HeapFree(void) { RET(tm_heap_free(ARG(0), ARG(1), ARG(2)), 3); }
static void shim_HeapSize(void) { RET(tm_heap_size(ARG(0), ARG(1), ARG(2)), 3); }
static void shim_HeapCompact(void) { RET(0x100000, 2); }
static void shim_HeapQueryInformation(void) { RET(0, 5); }
static void shim_GlobalAlloc(void) { RET(tm_heap_alloc(tm_heap_process(), (ARG(0) & 0x40) ? 8 : 0, ARG(1)), 2); }
static void shim_GlobalLock(void) { RET(ARG(0), 1); }
static void shim_GlobalUnlock(void) { RET(1, 1); }
static void shim_LocalFree(void) { tm_heap_free(tm_heap_process(), 0, ARG(0)); RET(0, 1); }

static void fill_memstatus(uint32_t p, int ex) {
    uint64_t total = 3ull << 30, avail = 2ull << 30;
    if (ex) {
        wr32(p + 4, 40);                    /* dwMemoryLoad */
        memcpy(G2H(p + 8), &total, 8);       /* TotalPhys */
        memcpy(G2H(p + 16), &avail, 8);
        uint64_t pf = 4ull << 30, pfa = 3ull << 30, tv = (4ull << 30) - (128u << 20), av = 3ull << 30, z = 0;
        memcpy(G2H(p + 24), &pf, 8); memcpy(G2H(p + 32), &pfa, 8);
        memcpy(G2H(p + 40), &tv, 8); memcpy(G2H(p + 48), &av, 8); memcpy(G2H(p + 56), &z, 8);
    } else {
        wr32(p + 0, 32); wr32(p + 4, 40);
        wr32(p + 8, 0xC0000000u); wr32(p + 12, 0x80000000u);
        wr32(p + 16, 0xFFFFFFFFu); wr32(p + 20, 0xC0000000u);
        wr32(p + 24, 0xF0000000u); wr32(p + 28, 0xC0000000u);
    }
}
static void shim_GlobalMemoryStatus(void) { fill_memstatus(ARG(0), 0); RET(0, 1); }
static void shim_GlobalMemoryStatusEx(void) { fill_memstatus(ARG(0), 1); RET(1, 1); }

/* ------------------------------------------------------------ TLS / FLS */
static void shim_TlsAlloc(void) { RET(tm_tls_alloc(), 0); }
static void shim_TlsFree(void) { RET(tm_tls_free(ARG(0)), 1); }
static void shim_TlsGetValue(void) { tm_set_last_error(0); RET(tm_tls_get(ARG(0)), 1); }
static void shim_TlsSetValue(void) { RET(tm_tls_set(ARG(0), ARG(1)), 2); }
/* FLS without callbacks: same slots (the fiber-less game never switches fibers). */
static void shim_FlsAlloc(void) { RET(tm_tls_alloc(), 1); }
static void shim_FlsFree(void) { RET(tm_tls_free(ARG(0)), 1); }
static void shim_FlsGetValue(void) { RET(tm_tls_get(ARG(0)), 1); }
static void shim_FlsSetValue(void) { RET(tm_tls_set(ARG(0), ARG(1)), 2); }

/* ------------------------------------------------------------ time */
static uint64_t filetime_now(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull;
}
static void shim_GetSystemTimeAsFileTime(void) { uint64_t t = filetime_now(); memcpy(G2H(ARG(0)), &t, 8); RET(0, 1); }
/* The "uptime" clocks (QueryPerformanceCounter, GetTickCount, timeGetTime) count from
 * a little before the game started, like a PC that was booted a minute ago, instead
 * of from the device's boot. TM_TIME_BASE=boot gives the raw monotonic clock.
 * (Diagnostic for now: large absolute values lose precision once code converts them
 * to float.) */
static uint64_t guest_uptime_ns(void) {
    static uint64_t origin; static int mode = -1;
    uint64_t now = tm_now_ns();
    if (mode < 0) {
        const char *e = getenv("TM_TIME_BASE");
        mode = e && !strcmp(e, "boot") ? 0 : 1;
        origin = mode ? now - 60ull * 1000000000ull : 0;
        if (origin > now) origin = 0;
    }
    return now - origin;
}
uint32_t tm_guest_tick_ms(void) { return (uint32_t)(guest_uptime_ns() / 1000000ull); }   /* what GetTickCount returns */
static void shim_QueryPerformanceCounter(void) { uint64_t t = guest_uptime_ns() / 100; memcpy(G2H(ARG(0)), &t, 8); RET(1, 1); }
static void shim_QueryPerformanceFrequency(void) { uint64_t f = 10000000ull; memcpy(G2H(ARG(0)), &f, 8); RET(1, 1); }
static void shim_GetTickCount(void) { RET((uint32_t)(guest_uptime_ns() / 1000000ull), 0); }
static void shim_GetTickCount64(void) { RET64(guest_uptime_ns() / 1000000ull, 0); }
static void shim_timeGetTime(void) { RET((uint32_t)(guest_uptime_ns() / 1000000ull), 0); }
static void shim_timeBeginPeriod(void) { RET(0, 1); }
static void shim_timeEndPeriod(void) { RET(0, 1); }
static void shim_timeGetDevCaps(void) { wr32(ARG(0), 1); wr32(ARG(0) + 4, 1000000); RET(0, 2); }

static void put_systemtime(uint32_t p, struct tm *t, int ms) {
    wr16(p + 0, (uint16_t)(t->tm_year + 1900)); wr16(p + 2, (uint16_t)(t->tm_mon + 1));
    wr16(p + 4, (uint16_t)t->tm_wday); wr16(p + 6, (uint16_t)t->tm_mday);
    wr16(p + 8, (uint16_t)t->tm_hour); wr16(p + 10, (uint16_t)t->tm_min);
    wr16(p + 12, (uint16_t)t->tm_sec); wr16(p + 14, (uint16_t)ms);
}
static void shim_GetLocalTime(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t; time_t s = ts.tv_sec; localtime_r(&s, &t);
    put_systemtime(ARG(0), &t, (int)(ts.tv_nsec / 1000000)); RET(0, 1);
}
static void shim_GetSystemTime(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t; time_t s = ts.tv_sec; gmtime_r(&s, &t);
    put_systemtime(ARG(0), &t, (int)(ts.tv_nsec / 1000000)); RET(0, 1);
}
static void shim_FileTimeToSystemTime(void) {
    uint64_t ft; memcpy(&ft, G2H(ARG(0)), 8);
    time_t s = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm t; gmtime_r(&s, &t);
    put_systemtime(ARG(1), &t, (int)((ft / 10000) % 1000)); RET(1, 2);
}
static void shim_SystemTimeToFileTime(void) {
    uint32_t p = ARG(0);
    struct tm t = {0};
    t.tm_year = rd16(p) - 1900; t.tm_mon = rd16(p + 2) - 1; t.tm_mday = rd16(p + 6);
    t.tm_hour = rd16(p + 8); t.tm_min = rd16(p + 10); t.tm_sec = rd16(p + 12);
    time_t s = timegm(&t);
    uint64_t ft = (uint64_t)s * 10000000ull + 116444736000000000ull + (uint64_t)rd16(p + 14) * 10000ull;
    memcpy(G2H(ARG(1)), &ft, 8); RET(1, 2);
}
static void shim_FileTimeToLocalFileTime(void) { memcpy(G2H(ARG(1)), G2H(ARG(0)), 8); RET(1, 2); }
static void shim_LocalFileTimeToFileTime(void) { memcpy(G2H(ARG(1)), G2H(ARG(0)), 8); RET(1, 2); }
static void shim_CompareFileTime(void) {
    uint64_t a, b; memcpy(&a, G2H(ARG(0)), 8); memcpy(&b, G2H(ARG(1)), 8);
    RET(a < b ? (uint32_t)-1 : a > b ? 1 : 0, 2);
}
static void shim_GetTimeZoneInformation(void) { memset(G2H(ARG(0)), 0, 172); RET(0, 1); }

/* ------------------------------------------------------------ system info */
static void fill_sysinfo(uint32_t p) {
    memset(G2H(p), 0, 36);
    wr16(p + 0, 0);              /* PROCESSOR_ARCHITECTURE_INTEL */
    wr32(p + 4, 4096);           /* page size */
    wr32(p + 8, 0x00010000);     /* min app address */
    wr32(p + 12, 0xFFFEFFFF);    /* max app address: LARGE_ADDRESS_AWARE on Win64 */
    wr32(p + 16, 0xF);           /* active processor mask */
    wr32(p + 20, 4);             /* processors */
    wr32(p + 24, 586);           /* PROCESSOR_INTEL_PENTIUM */
    wr32(p + 28, 0x10000);       /* allocation granularity */
    wr16(p + 32, 6); wr16(p + 34, 0x0F0B);
}
static void shim_GetSystemInfo(void) { fill_sysinfo(ARG(0)); RET(0, 1); }
static void shim_GetNativeSystemInfo(void) { fill_sysinfo(ARG(0)); RET(0, 1); }
static void shim_IsProcessorFeaturePresent(void) {
    uint32_t f = ARG(0);
    /* 0 FP errata,1 x87 ok? 2 cmpxchg 3 MMX 6 XMMI(SSE) 7 3DNow 8 RDTSC 9 PAE 10 SSE2 */
    int r = f == 3 || f == 6 || f == 8 || f == 10 || f == 2;
    RET(r, 1);
}
static void shim_GetVersion(void) { RET(0x1DB10106u, 0); /* 6.1 build 7601 */ }
static void shim_GetVersionExA(void) {
    uint32_t p = ARG(0); uint32_t sz = rd32(p);
    wr32(p + 4, 6); wr32(p + 8, 1); wr32(p + 12, 7601); wr32(p + 16, 2);
    memset(G2H(p + 20), 0, 128); strcpy((char *)G2H(p + 20), "Service Pack 1");
    if (sz >= 156) { wr16(p + 148, 1); wr16(p + 150, 0); wr16(p + 152, 0x100); wr8(p + 154, 1); wr8(p + 155, 0); }
    RET(1, 1);
}
static void shim_VerifyVersionInfoA(void) {
    /* Report Windows 7 SP1: >= checks up to 6.1 pass. */
    uint32_t p = ARG(0), mask = ARG(1);
    uint32_t maj = rd32(p + 4), min = rd32(p + 8);
    int ok = 1;
    if (mask & 2) ok = (6 > maj) || (6 == maj && ((mask & 1) ? 1 >= min : 1));
    if (!ok) tm_set_last_error(1150);
    RET(ok, 4);
}
static void shim_VerifyVersionInfoW(void) { shim_VerifyVersionInfoA(); }
static void shim_VerSetConditionMask(void) {
    /* ULONGLONG VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE cond): args = lo, hi, type, cond */
    uint64_t m = ARG(0) | ((uint64_t)ARG(1) << 32);
    uint32_t type = ARG(2), cond = ARG(3) & 7;
    for (int i = 0; i < 8; i++) if (type & (1u << i)) m |= (uint64_t)cond << (i * 3);
    RET64(m, 4);
}
static void shim_GetProcessAffinityMask(void) { if (ARG(1)) wr32(ARG(1), 0xF); if (ARG(2)) wr32(ARG(2), 0xF); RET(1, 3); }
static void shim_SetThreadAffinityMask(void) { RET(0xF, 2); }
static void shim_SetPriorityClass(void) { RET(1, 2); }
static void shim_GetPriorityClass(void) { RET(0x20, 1); }
static void shim_SetThreadPriority(void) { int *p = tm_thread_priority_ptr(ARG(0)); if (p) *p = (int)ARG(1); RET(p != NULL, 2); }
static void shim_GetThreadPriority(void) { int *p = tm_thread_priority_ptr(ARG(0)); RET(p ? (uint32_t)*p : 0x7FFFFFFFu, 1); }

/* ------------------------------------------------------------ sync objects */
static uint32_t new_sync(int kind, int manual, int initial, int count, int max) {
    tm_object *o = calloc(1, sizeof *o);
    o->kind = kind; o->manual_reset = manual; o->signaled = initial;
    o->count = count; o->max_count = max;
    return tm_handle_new(o);
}

/* named objects: the game uses names for single-instance checks only */
typedef struct { char name[128]; uint32_t h; } named_obj;
static named_obj g_named[64];
static int g_nnamed;
static uint32_t named_lookup(const char *n) {
    for (int i = 0; i < g_nnamed; i++) if (!strcmp(g_named[i].name, n)) return g_named[i].h;
    return 0;
}
static void named_add(const char *n, uint32_t h) {
    if (g_nnamed < 64) { snprintf(g_named[g_nnamed].name, 128, "%s", n); g_named[g_nnamed].h = h; g_nnamed++; }
}
static uint32_t create_named(const char *name, int kind, int manual, int initial, int count, int max) {
    if (name && *name) {
        uint32_t h = named_lookup(name);
        if (h) { tm_set_last_error(ERROR_ALREADY_EXISTS); return tm_handle_dup(h); }
    }
    uint32_t h = new_sync(kind, manual, initial, count, max);
    if (name && *name) named_add(name, h);
    tm_set_last_error(0);
    return h;
}

static void shim_CreateEventA(void) { RET(create_named(ARGS(3), TMO_EVENT, ARG(1) != 0, ARG(2) != 0, 0, 0), 4); }
static void shim_CreateEventW(void) { char n[256]; RET(create_named(ARG(3) ? tm_w2u(ARG(3), n, sizeof n) : NULL, TMO_EVENT, ARG(1) != 0, ARG(2) != 0, 0, 0), 4); }
static void shim_OpenEventW(void) { char n[256]; uint32_t h = named_lookup(tm_w2u(ARG(2), n, sizeof n)); if (!h) tm_set_last_error(2); RET(h ? tm_handle_dup(h) : 0, 3); }
static void shim_CreateMutexA(void) {
    uint32_t h = create_named(ARGS(2), TMO_MUTEX, 0, 1, 0, 0);
    if (ARG(1) && tm_get_last_error() != ERROR_ALREADY_EXISTS) {
        tm_object *o = tm_handle_get(h, TMO_MUTEX); o->signaled = 0; o->owner_tid = tm_thread_id(); o->recursion = 1;
    }
    RET(h, 3);
}
static void shim_CreateSemaphoreA(void) { RET(create_named(ARGS(3), TMO_SEMAPHORE, 0, 0, (int)ARG(1), (int)ARG(2)), 4); }
static void shim_CreateSemaphoreW(void) { char n[256]; RET(create_named(ARG(3) ? tm_w2u(ARG(3), n, sizeof n) : NULL, TMO_SEMAPHORE, 0, 0, (int)ARG(1), (int)ARG(2)), 4); }
static void shim_CreateSemaphoreExW(void) { char n[256]; RET(create_named(ARG(3) ? tm_w2u(ARG(3), n, sizeof n) : NULL, TMO_SEMAPHORE, 0, 0, (int)ARG(1), (int)ARG(2)), 6); }

static void shim_SetEvent(void) {
    tm_object *o = tm_handle_get(ARG(0), TMO_EVENT);
    if (!o) { tm_set_last_error(ERROR_INVALID_HANDLE); RET(0, 1); }
    tm_sync_lock(); o->signaled = 1; tm_sync_wake(o); tm_sync_unlock();
    RET(1, 1);
}
static void shim_ResetEvent(void) {
    tm_object *o = tm_handle_get(ARG(0), TMO_EVENT);
    if (!o) { tm_set_last_error(ERROR_INVALID_HANDLE); RET(0, 1); }
    tm_sync_lock(); o->signaled = 0; tm_sync_unlock();
    RET(1, 1);
}
static void shim_ReleaseSemaphore(void) {
    tm_object *o = tm_handle_get(ARG(0), TMO_SEMAPHORE);
    if (!o) { tm_set_last_error(ERROR_INVALID_HANDLE); RET(0, 3); }
    tm_sync_lock();
    int prev = o->count;
    int ok = o->count + (int)ARG(1) <= o->max_count;
    if (ok) o->count += (int)ARG(1);
    tm_sync_wake(o); tm_sync_unlock();
    if (ARG(2)) wr32(ARG(2), (uint32_t)prev);
    RET(ok, 3);
}
static void shim_ReleaseMutex(void) {
    tm_object *o = tm_handle_get(ARG(0), TMO_MUTEX);
    if (!o) RET(0, 1);
    tm_sync_lock();
    int ok = o->owner_tid == tm_thread_id() && o->recursion > 0;
    if (ok && --o->recursion == 0) { o->owner_tid = 0; o->signaled = 1; tm_sync_wake(o); }
    tm_sync_unlock();
    RET(ok, 1);
}

/* Must be called with the sync mutex held. Returns 1 and consumes if signaled. */
static int try_acquire(tm_object *o) {
    switch (o->kind) {
    case TMO_EVENT: if (!o->signaled) return 0; if (!o->manual_reset) o->signaled = 0; return 1;
    case TMO_SEMAPHORE: if (o->count <= 0) return 0; o->count--; return 1;
    case TMO_MUTEX:
        if (o->owner_tid == tm_thread_id()) { o->recursion++; return 1; }
        if (o->owner_tid) return 0;
        o->owner_tid = tm_thread_id(); o->recursion = 1; o->signaled = 0; return 1;
    case TMO_THREAD: case TMO_PROCESS: return o->signaled;
    default: return 1;  /* files, etc. count as signaled */
    }
}
static int is_signaled(tm_object *o) {
    switch (o->kind) {
    case TMO_EVENT: return o->signaled;
    case TMO_SEMAPHORE: return o->count > 0;
    case TMO_MUTEX: return !o->owner_tid || o->owner_tid == tm_thread_id();
    case TMO_THREAD: case TMO_PROCESS: return o->signaled;
    default: return 1;
    }
}

#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu

uint32_t tm_wait_multiple(uint32_t n, const uint32_t *hs, int all, uint32_t ms) {
    tm_object *objs[64];
    if (n == 0 || n > 64) return WAIT_FAILED;
    for (uint32_t i = 0; i < n; i++) {
        if (hs[i] == TM_PSEUDO_THREAD || hs[i] == TM_PSEUDO_PROCESS) { objs[i] = NULL; continue; }
        objs[i] = tm_handle_get(hs[i], 0);
        if (!objs[i]) { TM_WARN("wait on invalid handle %08X from %08X", hs[i], g_cur_func); tm_set_last_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    }
    uint64_t start = tm_now_ns();
    uint32_t result = WAIT_TIMEOUT;
    int released = 0;
    tm_sync_lock();
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) if (objs[i] && !is_signaled(objs[i])) ok = 0;
            if (ok) { for (uint32_t i = 0; i < n; i++) if (objs[i]) try_acquire(objs[i]); result = 0; break; }
        } else {
            int hit = -1;
            for (uint32_t i = 0; i < n; i++) if (objs[i] && try_acquire(objs[i])) { hit = (int)i; break; }
            if (hit >= 0) { result = (uint32_t)hit; break; }
        }
        if (ms == 0) break;
        if (!released) {
            /* Hand the machine to other guest threads while blocked. Lock order:
             * the machine lock is never taken while holding the sync mutex. */
            tm_sync_unlock();
            tm_machine_unlock();
            released = 1;
            tm_sync_lock();
            continue;
        }
        if (!tm_sync_wait_keys((const void *const *)objs, (int)n, ms, start)) { /* timed out: one last check */
            if (!all) { for (uint32_t i = 0; i < n; i++) if (objs[i] && try_acquire(objs[i])) { result = i; break; } }
            break;
        }
    }
    tm_sync_unlock();
    if (released) tm_machine_lock();
    return result;
}

static void shim_WaitForSingleObject(void) {
    uint32_t h = ARG(0);
    RET(tm_wait_multiple(1, &h, 0, ARG(1)), 2);
}
static void shim_WaitForSingleObjectEx(void) {
    uint32_t h = ARG(0);
    RET(tm_wait_multiple(1, &h, 0, ARG(1)), 3);
}
static void shim_WaitForMultipleObjects(void) {
    uint32_t n = ARG(0), arr = ARG(1), hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = rd32(arr + 4 * i);
    RET(tm_wait_multiple(n, hs, ARG(2) != 0, ARG(3)), 4);
}

static void shim_CloseHandle(void) {
    uint32_t h = ARG(0);
    if (h == TM_PSEUDO_PROCESS || h == TM_PSEUDO_THREAD) RET(1, 1);
    int ok = tm_handle_close(h);
    if (!ok) { TM_TRACE("CloseHandle(%08X) invalid", h); tm_set_last_error(ERROR_INVALID_HANDLE); }
    RET(ok, 1);
}
static void shim_DuplicateHandle(void) {
    uint32_t src = ARG(1), out = ARG(3);
    uint32_t h = src == TM_PSEUDO_THREAD || src == TM_PSEUDO_PROCESS ? src : tm_handle_dup(src);
    if (out) wr32(out, h);
    RET(h != 0, 7);
}

/* ------------------------------------------------------------ critical sections
 * CRITICAL_SECTION (24 bytes): DebugInfo, LockCount, RecursionCount,
 * OwningThread, LockSemaphore, SpinCount. Guest threads are serialised by the
 * machine lock, so ownership checks need no atomics; contention waits on the
 * shared condition variable with the machine released. */
static void cs_init(uint32_t cs) { memset(G2H(cs), 0, 24); wr32(cs + 4, 0xFFFFFFFFu); }
static void shim_InitializeCriticalSection(void) { cs_init(ARG(0)); RET(0, 1); }
static void shim_InitializeCriticalSectionAndSpinCount(void) { cs_init(ARG(0)); RET(1, 2); }
static void shim_InitializeCriticalSectionEx(void) { cs_init(ARG(0)); RET(1, 3); }
static void shim_DeleteCriticalSection(void) { RET(0, 1); }
static void shim_SetCriticalSectionSpinCount(void) { RET(0, 2); }

static uint32_t g_cs_gen;   /* bumped (under the sync mutex) by every final Leave */
static void cs_enter(uint32_t cs) {
    uint32_t me = tm_thread_id();
    uint32_t owner = rd32(cs + 12);
    if (owner == me) { wr32(cs + 8, rd32(cs + 8) + 1); return; }
    while (rd32(cs + 12)) {
        /* Read the generation before giving up the machine, so a Leave that
         * runs in between is never missed (the sync mutex is held until the
         * condition wait releases it). */
        const void *key = G2H(cs);
        tm_sync_lock();
        uint32_t gen = g_cs_gen;
        wr32(cs + 16, rd32(cs + 16) + 1);          /* waiter count: Leave only signals a contended section */
        tm_machine_unlock();
        uint64_t start = tm_now_ns();
        while (g_cs_gen == gen) if (!tm_sync_wait_keys(&key, 1, 50, start)) break;
        tm_sync_unlock();
        tm_machine_lock();
        wr32(cs + 16, rd32(cs + 16) - 1);
    }
    wr32(cs + 12, me);
    wr32(cs + 8, 1);
    wr32(cs + 4, 0);
}
static void shim_EnterCriticalSection(void) { cs_enter(ARG(0)); RET(0, 1); }
static void shim_TryEnterCriticalSection(void) {
    uint32_t cs = ARG(0), me = tm_thread_id(), owner = rd32(cs + 12);
    if (owner == me) { wr32(cs + 8, rd32(cs + 8) + 1); RET(1, 1); }
    if (owner) RET(0, 1);
    wr32(cs + 12, me); wr32(cs + 8, 1); wr32(cs + 4, 0);
    RET(1, 1);
}
static void shim_LeaveCriticalSection(void) {
    uint32_t cs = ARG(0);
    if (rd32(cs + 12) != tm_thread_id()) {
        TM_WARN("LeaveCriticalSection %08X not owned (owner %u) from %08X", cs, rd32(cs + 12), g_cur_func);
        RET(0, 1);
    }
    uint32_t r = rd32(cs + 8) - 1;
    wr32(cs + 8, r);
    if (!r) {
        wr32(cs + 12, 0); wr32(cs + 4, 0xFFFFFFFFu);
        /* Waiters announce themselves in the LockSemaphore field while they still
         * hold the machine, so an uncontended Leave (nearly all of them, thousands
         * per frame) takes no lock and wakes nobody. */
        extern int tm_sync_legacy;
        if (rd32(cs + 16) || tm_sync_legacy > 0) { tm_sync_lock(); g_cs_gen++; tm_sync_wake(G2H(cs)); tm_sync_unlock(); }
    }
    RET(0, 1);
}

/* Condition variables (CONDITION_VARIABLE is one pointer) */
static void shim_InitializeConditionVariable(void) { wr32(ARG(0), 0); RET(0, 1); }
static void shim_WakeConditionVariable(void) {
    tm_sync_lock(); wr32(ARG(0), rd32(ARG(0)) + 1); tm_sync_wake(G2H(ARG(0))); tm_sync_unlock(); RET(0, 1);
}
static void shim_SleepConditionVariableCS(void) {
    uint32_t cv = ARG(0), cs = ARG(1), ms = ARG(2);
    uint32_t gen = rd32(cv);
    uint32_t rec = rd32(cs + 8);
    /* leave CS fully */
    wr32(cs + 8, 0); wr32(cs + 12, 0); wr32(cs + 4, 0xFFFFFFFFu);
    tm_sync_lock(); g_cs_gen++; tm_sync_wake(G2H(cs)); tm_sync_unlock();
    uint64_t start = tm_now_ns();
    int signaled = 0;
    const void *key = G2H(cv);
    tm_machine_unlock();
    tm_sync_lock();
    for (;;) {
        if (rd32(cv) != gen) { signaled = 1; break; }
        if (!tm_sync_wait_keys(&key, 1, ms, start)) break;
    }
    tm_sync_unlock();
    tm_machine_lock();
    cs_enter(cs);
    wr32(cs + 8, rec);
    if (!signaled) { tm_set_last_error(0x5B4); RET(0, 3); }
    RET(1, 3);
}

/* Interlocked SLists (SLIST_HEADER: Next, Depth/Sequence) */
static void shim_InitializeSListHead(void) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 0); RET(0, 1); }
static void shim_InterlockedPushEntrySList(void) {
    uint32_t h = ARG(0), e = ARG(1), old = rd32(h);
    wr32(e, old); wr32(h, e); wr16(h + 4, (uint16_t)(rd16(h + 4) + 1));
    RET(old, 2);
}
static void shim_InterlockedPopEntrySList(void) {
    uint32_t h = ARG(0), e = rd32(h);
    if (e) { wr32(h, rd32(e)); wr16(h + 4, (uint16_t)(rd16(h + 4) - 1)); }
    RET(e, 1);
}
/* Interlocked*: the machine lock makes plain read-modify-write atomic for guests */
static void shim_InterlockedIncrement(void) { uint32_t v = rd32(ARG(0)) + 1; wr32(ARG(0), v); RET(v, 1); }
static void shim_InterlockedDecrement(void) { uint32_t v = rd32(ARG(0)) - 1; wr32(ARG(0), v); RET(v, 1); }
static void shim_InterlockedExchange(void) { uint32_t o = rd32(ARG(0)); wr32(ARG(0), ARG(1)); RET(o, 2); }
static void shim_InterlockedExchangeAdd(void) { uint32_t o = rd32(ARG(0)); wr32(ARG(0), o + ARG(1)); RET(o, 2); }
static void shim_InterlockedCompareExchange(void) {
    uint32_t o = rd32(ARG(0)); if (o == ARG(2)) wr32(ARG(0), ARG(1)); RET(o, 3);
}

/* ------------------------------------------------------------ threads */
static void shim_CreateThread(void) {
    uint32_t tid = 0;
    uint32_t h = tm_create_thread(ARG(1), ARG(2), ARG(3), ARG(4), &tid);
    if (ARG(5)) wr32(ARG(5), tid);
    TM_INFO("CreateThread(start %08X, param %08X, flags %X) -> handle %08X tid %u", ARG(2), ARG(3), ARG(4), h, tid);
    RET(h, 6);
}
static void shim_ExitThread(void) { tm_exit_thread(ARG(0)); }
static void shim_ResumeThread(void) { uint32_t prev = 0; int ok = tm_resume_thread(ARG(0), &prev); RET(ok ? prev : 0xFFFFFFFFu, 1); }
static void shim_SuspendThread(void) {
    TM_WARN("SuspendThread(%08X) from %08X ignored", ARG(0), g_cur_func);
    RET(0, 1);
}
static void shim_GetExitCodeThread(void) { int ok; uint32_t c = tm_thread_exit_code(ARG(0), &ok); if (ok && ARG(1)) wr32(ARG(1), c); RET(ok, 2); }
static void shim_GetThreadId(void) { RET(tm_thread_handle_tid(ARG(0)), 1); }
static void shim_Sleep(void) {
    uint32_t ms = ARG(0);
    tm_machine_unlock();
    if (ms == 0) sched_yield();
    else { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
    tm_machine_lock();
    RET(0, 1);
}
static void shim_SleepEx(void) {
    uint32_t ms = ARG(0);
    tm_machine_unlock();
    if (ms == 0) sched_yield();
    else { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
    tm_machine_lock();
    RET(0, 2);
}
static void shim_SwitchToThread(void) { tm_machine_unlock(); sched_yield(); tm_machine_lock(); RET(1, 0); }

/* ------------------------------------------------------------ console */
static void shim_GetStdHandle(void) { uint32_t n = ARG(0); RET(n == (uint32_t)-10 ? 0x10u : n == (uint32_t)-11 ? 0x14u : n == (uint32_t)-12 ? 0x18u : 0xFFFFFFFFu, 1); }
static void shim_SetStdHandle(void) { RET(1, 2); }
static void shim_GetConsoleMode(void) { RET(0, 2); }
static void shim_SetConsoleMode(void) { RET(0, 2); }
static void shim_GetConsoleCP(void) { RET(437, 0); }
static void shim_AllocConsole(void) { RET(0, 0); }
static void shim_FreeConsole(void) { RET(1, 0); }
static void shim_SetConsoleCtrlHandler(void) { RET(1, 2); }
static void shim_WriteConsoleA(void) { fwrite(G2H(ARG(1)), 1, ARG(2), stderr); if (ARG(3)) wr32(ARG(3), ARG(2)); RET(1, 5); }
static void shim_WriteConsoleW(void) { char b[4096]; fputs(tm_w2u_n(ARG(1), (int)ARG(2), b, sizeof b), stderr); if (ARG(3)) wr32(ARG(3), ARG(2)); RET(1, 5); }
static void shim_FlushConsoleInputBuffer(void) { RET(1, 1); }
static void shim_PeekNamedPipe(void) { RET(0, 6); }

/* ------------------------------------------------------------ resources
 * Minimal PE resource walker over the mapped .rsrc. */
static uint32_t rsrc_find_entry(uint32_t dir, uint32_t id_or_name) {
    uint32_t base = g_image.rsrc_va;
    uint16_t nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    uint32_t e = dir + 16;
    for (uint32_t i = 0; i < (uint32_t)nnamed + nid; i++, e += 8) {
        uint32_t name = rd32(e), off = rd32(e + 4);
        int match;
        if (id_or_name < 0x10000) match = !(name & 0x80000000u) && name == id_or_name;
        else {
            if (!(name & 0x80000000u)) continue;
            uint32_t s = base + (name & 0x7FFFFFFFu);
            uint16_t len = rd16(s);
            match = 1;
            for (uint16_t k = 0; k < len; k++) {
                uint16_t a = rd16(s + 2 + 2 * k), b = rd16(id_or_name + 2 * k);
                if (toupper(a) != toupper(b)) { match = 0; break; }
            }
            if (match && rd16(id_or_name + 2 * len)) match = 0;
        }
        if (match) return off;
    }
    return 0xFFFFFFFFu;
}
uint32_t tm_find_resource_entry(uint32_t type, uint32_t name);
static uint32_t find_resource(uint32_t type, uint32_t name, int lang_any) {
    (void)lang_any;
    if (!g_image.rsrc_va) return 0;
    uint32_t base = g_image.rsrc_va;
    uint32_t o = rsrc_find_entry(base, type);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = rsrc_find_entry(base + (o & 0x7FFFFFFFu), name);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    uint32_t langdir = base + (o & 0x7FFFFFFFu);
    if (rd16(langdir + 12) + rd16(langdir + 14) == 0) return 0;
    uint32_t data = rd32(langdir + 16 + 4);
    if (data & 0x80000000u) return 0;
    return base + data;   /* IMAGE_RESOURCE_DATA_ENTRY */
}
uint32_t tm_find_resource_entry(uint32_t type, uint32_t name) { return find_resource(type, name, 1); }
static void shim_FindResourceW(void) { RET(find_resource(ARG(2), ARG(1), 1), 3); }
static void shim_FindResourceExW(void) { RET(find_resource(ARG(1), ARG(2), 1), 4); }
static void shim_LoadResource(void) { uint32_t e = ARG(1); RET(e ? g_image.image_base + rd32(e) : 0, 2); }
static void shim_LockResource(void) { RET(ARG(0), 1); }
static void shim_SizeofResource(void) { uint32_t e = ARG(1); RET(e ? rd32(e + 4) : 0, 2); }

static const tm_shim_def k32_core_shims[] = {
#define K(n) TM_SHIM("kernel32.dll", n)
    K(GetCommandLineA), K(GetCommandLineW), K(GetModuleHandleA), K(GetModuleHandleW), K(GetModuleHandleExW),
    K(SetHandleCount), K(EnumSystemLocalesA),
    K(GetModuleFileNameA), K(GetModuleFileNameW), K(LoadLibraryA), K(LoadLibraryW), K(LoadLibraryExW), K(LoadLibraryExA),
    K(FreeLibrary), K(GetProcAddress), K(GetCurrentProcess), K(GetCurrentProcessId), K(GetCurrentThread),
    K(GetCurrentThreadId), K(ExitProcess), K(TerminateProcess), K(GetStartupInfoW), K(GetStartupInfoA),
    K(GetEnvironmentStringsW), K(FreeEnvironmentStringsW), K(SetEnvironmentVariableA), K(ExpandEnvironmentStringsA),
    K(GetLastError), K(SetLastError), K(SetErrorMode), K(UnhandledExceptionFilter), K(SetUnhandledExceptionFilter),
    K(IsDebuggerPresent), K(DebugBreak), K(OutputDebugStringA), K(OutputDebugStringW), K(EncodePointer), K(DecodePointer),
    K(RaiseException), K(RtlUnwind), K(RtlCaptureStackBackTrace), K(IsBadReadPtr), K(IsBadWritePtr), K(IsBadCodePtr),
    K(FormatMessageA), K(VirtualAlloc), K(VirtualFree), K(VirtualQuery), K(VirtualProtect), K(GetProcessHeap),
    K(HeapCreate), K(HeapDestroy), K(HeapAlloc), K(HeapReAlloc), K(HeapFree), K(HeapSize), K(HeapCompact),
    K(HeapQueryInformation), K(GlobalAlloc), K(GlobalLock), K(GlobalUnlock), K(LocalFree), K(GlobalMemoryStatus),
    K(GlobalMemoryStatusEx), K(TlsAlloc), K(TlsFree), K(TlsGetValue), K(TlsSetValue), K(FlsAlloc), K(FlsFree),
    K(FlsGetValue), K(FlsSetValue), K(GetSystemTimeAsFileTime), K(QueryPerformanceCounter), K(QueryPerformanceFrequency),
    K(GetTickCount), K(GetTickCount64), K(GetLocalTime), K(GetSystemTime), K(FileTimeToSystemTime),
    K(SystemTimeToFileTime), K(FileTimeToLocalFileTime), K(LocalFileTimeToFileTime), K(CompareFileTime),
    K(GetTimeZoneInformation), K(GetSystemInfo), K(GetNativeSystemInfo), K(IsProcessorFeaturePresent), K(GetVersion),
    K(GetVersionExA), K(VerifyVersionInfoA), K(VerifyVersionInfoW), K(VerSetConditionMask), K(GetProcessAffinityMask),
    K(SetThreadAffinityMask), K(SetPriorityClass), K(GetPriorityClass), K(SetThreadPriority), K(GetThreadPriority),
    K(CreateEventA), K(CreateEventW), K(OpenEventW), K(CreateMutexA), K(CreateSemaphoreA), K(CreateSemaphoreW),
    K(CreateSemaphoreExW), K(SetEvent), K(ResetEvent), K(ReleaseSemaphore), K(ReleaseMutex), K(WaitForSingleObject),
    K(WaitForSingleObjectEx), K(WaitForMultipleObjects), K(CloseHandle), K(DuplicateHandle),
    K(InitializeCriticalSection), K(InitializeCriticalSectionAndSpinCount), K(InitializeCriticalSectionEx),
    K(DeleteCriticalSection), K(SetCriticalSectionSpinCount), K(EnterCriticalSection), K(TryEnterCriticalSection),
    K(LeaveCriticalSection), K(InitializeConditionVariable), K(WakeConditionVariable), K(SleepConditionVariableCS),
    K(InitializeSListHead), K(InterlockedPushEntrySList), K(InterlockedPopEntrySList), K(InterlockedIncrement),
    K(InterlockedDecrement), K(InterlockedExchange), K(InterlockedExchangeAdd), K(InterlockedCompareExchange),
    K(CreateThread), K(ExitThread), K(ResumeThread), K(SuspendThread), K(GetExitCodeThread), K(GetThreadId),
    K(Sleep), K(SleepEx), K(SwitchToThread), K(GetStdHandle), K(SetStdHandle), K(GetConsoleMode), K(SetConsoleMode),
    K(GetConsoleCP), K(AllocConsole), K(FreeConsole), K(SetConsoleCtrlHandler), K(WriteConsoleA), K(WriteConsoleW),
    K(FlushConsoleInputBuffer), K(PeekNamedPipe), K(FindResourceW), K(FindResourceExW), K(LoadResource),
    K(LockResource), K(SizeofResource),
#undef K
    TM_SHIM("winmm.dll", timeGetTime), TM_SHIM("winmm.dll", timeBeginPeriod), TM_SHIM("winmm.dll", timeEndPeriod),
    TM_SHIM("winmm.dll", timeGetDevCaps),
};

void tm_k32_core_register(void) { tm_register_shims(k32_core_shims, sizeof k32_core_shims / sizeof k32_core_shims[0]); }
