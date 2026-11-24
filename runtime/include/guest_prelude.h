/* Force-included into every generated translation unit (never edited by hand
 * in generated files): host replacements for RDTSC and CPUID. */
#pragma once
#include <time.h>
#define RECOMP_GENERATED_CODE
/* Out-of-range x87 integer stores call recomp_fist_oor() (runtime/src/core.c), which
 * can log them (TM_FIST_LOG) or apply another convention (TM_FIST_SATURATE). */
#define RECOMP_FIST_HOOK
/* Lifted functions keep the state pointer and the guest memory base in locals
 * (see recomp_types.h); switched on at the end of this file. */
#define RECOMP_LOCAL_STATE
#include "recomp_types.h"
static inline uint64_t tm_guest_rdtsc(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    /* virtual 1 GHz counter */
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define __rdtsc() tm_guest_rdtsc()
/* Virtual CPU: Pentium 4 class, x87/TSC/CX8/CMOV/MMX/SSE/SSE2 only, so every
 * CPUID-dispatched SSSE3/SSE4/AVX path in the game takes its SSE2 fallback. */
static inline void tm_guest_cpuid(uint32_t leaf) {
    g_eax = g_ebx = g_ecx = g_edx = 0;
    if (leaf == 0) { g_eax = 1; g_ebx = 0x756e6547; g_edx = 0x49656e69; g_ecx = 0x6c65746e; }
    else if (leaf == 1) { g_eax = 0x00000F29; g_ebx = 0x00040800;
        g_edx = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26); }
    else if (leaf == 0x80000000u) g_eax = 0x80000001u;
}
#undef CPUID
#define CPUID(a,b,c,d) do { RECOMP_REGS_OUT(); tm_guest_cpuid((a)); RECOMP_REGS_IN(); } while (0)

/* Must stay last: from here on (the lifted function bodies) the state pointer and the
 * memory base are the function's own locals. */
#include "recomp_local_state.h"
