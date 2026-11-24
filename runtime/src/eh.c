/*
 * eh.c - C++ exceptions of the game (MSVC x86 "throw" / "catch").
 *
 * The game throws very few C++ exceptions, but its network code relies on them: every
 * socket error and every connection the other side closes ends in
 *   throw CNetSystem::CNetSystemError(...)            (0x0051A460 recv, 0x0051B210 accept, ...)
 * caught a few calls up (CNetSystem::UpdateSockets and friends), which drop the
 * connection and go on. Without this a LAN game ends when a player leaves.
 *
 * MSVC x86 EH in short: a function with try blocks or destructors registers a node on
 * the thread's SEH chain (fs:[0]): { next, handler thunk, state }; the thunk does
 * "mov eax, FuncInfo ; jmp __CxxFrameHandler3". FuncInfo = { magic, maxState,
 * pUnwindMap {toState, action}[], nTryBlocks, pTryBlockMap {tryLow, tryHigh,
 * catchHigh, nCatches, pHandlers {adjectives, pType, dispCatchObj, funclet}[]}[] }.
 * The function's frame pointer for its funclets is node + 0xC, and its stack pointer
 * inside the body is saved at [frame - 0x10]. throw = RaiseException(0xE06D7363, 1,
 * 3, { 0x19930520, object, ThrowInfo }); ThrowInfo = { attributes, pmfnUnwind
 * (destructor), forward compat, pCatchableTypeArray {n, CatchableType {properties,
 * pType, thisDisplacement, size, copyFunction}[]} }.
 *
 * The recompiled game cannot jump into the middle of a C function, so a catch is done
 * like this:
 *   - every function with try blocks is lifted as a stub that sets a host anchor
 *     (setjmp) and calls the body (tools/run_lift.py, config/eh_wrap.txt from
 *     work/eh_funcs.py; the table of those functions is eh_table.inc);
 *   - RaiseException walks the SEH chain as __CxxFrameHandler3 would: finds the first
 *     try block whose state range holds the node's state and that has a handler for the
 *     thrown type (or catch(...)), runs the destructors of the frames it leaves
 *     (unwind funclets, frame pointer node + 0xC) and of the try block's own objects,
 *     builds the catch object, calls the catch funclet (it returns the continuation
 *     address), destroys the exception object and unlinks the left frames;
 *   - then it longjmps to the anchor of that function's call, which sets esp/ebp to
 *     the function's frame and runs the lifted code at the continuation address (the
 *     catalog has those as entries) up to the function's own "ret".
 * Frames without C++ EH (SEH __try in the CRT) are skipped, as their filters would
 * not take a C++ exception anyway. "throw;" inside a catch rethrows the current one.
 */
#include "tmrt.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct eh_anchor {
    jmp_buf jb;
    uint32_t va, esp, funcinfo;      /* the wrapped function, esp at its entry (-> return address) */
    uint32_t cont, frame;            /* set by the dispatcher before longjmp */
    struct eh_anchor *up;
} eh_anchor;

static __thread eh_anchor *t_top;
static __thread uint32_t t_cur_obj, t_cur_ti;   /* exception being handled (for "throw;") */

static const uint32_t g_table[][2] = {
#include "eh_table.inc"
};
#define N_TABLE (sizeof g_table / sizeof g_table[0])

static uint32_t funcinfo_of_fn(uint32_t va) {
    size_t lo = 0, hi = N_TABLE;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_table[m][0] < va) lo = m + 1; else hi = m; }
    return lo < N_TABLE && g_table[lo][0] == va ? g_table[lo][1] : 0;
}

/* The FuncInfo of a frame handler thunk (mov eax, imm32 ; jmp), 0 if it is none. */
static uint32_t thunk_funcinfo(uint32_t h) {
    if (h < 0x00401000u || h >= 0x016A3000u) return 0;            /* the exe's .text */
    for (uint32_t k = 0; k < 0x3B; k++) {
        if (rd8(h + k) == 0xB8 && rd8(h + k + 5) == 0xE9) {
            uint32_t fi = rd32(h + k + 1), magic = rd32(fi);
            if (fi >= 0x016A3000u && fi < 0x01C00000u && (magic == 0x19930520u || magic == 0x19930521u || magic == 0x19930522u)) return fi;
        }
        if (rd8(h + k) == 0xC3) break;
    }
    return 0;
}

static const char *type_name(uint32_t td) { return td ? (const char *)G2H(td + 8) : "..."; }

/* Runs a funclet with ebp = frame; returns eax (the continuation of a catch funclet). */
static uint32_t call_funclet(uint32_t fn, uint32_t frame) {
    uint32_t saved = g_ebp;
    g_ebp = frame;
    uint32_t r = tm_call_guest(fn, 0);
    g_ebp = saved;
    return r;
}

/* Destructors of a frame from its current state down to `target` (unwind map). */
static void unwind_to(uint32_t rn, uint32_t fi, int32_t target) {
    int32_t maxs = (int32_t)rd32(fi + 4), st = (int32_t)rd32(rn + 8);
    uint32_t um = rd32(fi + 8);
    int guard = 0;
    while (st != target && st >= 0 && st < maxs && guard++ < 4096) {
        int32_t to = (int32_t)rd32(um + 8u * (uint32_t)st);
        uint32_t action = rd32(um + 8u * (uint32_t)st + 4);
        wr32(rn + 8, (uint32_t)to);
        if (action) call_funclet(action, rn + 0xC);
        st = to;
    }
}

void tm_eh_wrap(uint32_t va, void (*body)(void)) {
    eh_anchor a;
    a.va = va; a.esp = g_esp; a.up = t_top; a.funcinfo = 0; a.cont = 0; a.frame = 0;
    t_top = &a;
    if (setjmp(a.jb) == 0) {
        body();
        t_top = a.up;
        return;
    }
    /* a catch in this call: go on at the continuation, in the function's frame */
    for (;;) {
        recomp_func_t f = recomp_lookup(a.cont);
        if (!f) tm_fatal("eh: continuation %08X of %08X is not a lifted entry", a.cont, va);
        g_ebp = a.frame;
        g_esp = rd32(a.frame - 0x10);
        t_top = &a;
        if (setjmp(a.jb) == 0) { f(); break; }       /* another catch in the same call: loop */
    }
    t_top = a.up;
}

/* Called by RaiseException for a C++ exception. Returns only if no handler was found. */
void tm_eh_raise(uint32_t obj, uint32_t ti, uint32_t from) {
    static int logged;
    if (!obj && !ti) { obj = t_cur_obj; ti = t_cur_ti; }             /* "throw;" */
    if (!ti) return;
    uint32_t cta = rd32(ti + 12), ntypes = cta ? rd32(cta) : 0;
    const char *tname = ntypes ? type_name(rd32(rd32(cta + 4) + 4)) : "?";
    uint32_t teb = tm_thread_teb();
    if (!teb) return;
    for (uint32_t rn = rd32(teb); rn && rn != 0xFFFFFFFFu; rn = rd32(rn)) {
        uint32_t fi = thunk_funcinfo(rd32(rn + 4));
        if (!fi) continue;
        int32_t st = (int32_t)rd32(rn + 8);
        uint32_t ntry = rd32(fi + 12), ptry = rd32(fi + 16);
        for (uint32_t t = 0; t < ntry; t++) {
            uint32_t e = ptry + 20 * t;
            int32_t lo = (int32_t)rd32(e), hi = (int32_t)rd32(e + 4);
            if (st < lo || st > hi) continue;
            uint32_t nc = rd32(e + 12), ph = rd32(e + 16);
            for (uint32_t h = 0; h < nc; h++) {
                uint32_t hd = ph + 16 * h, adj = rd32(hd), ptype = rd32(hd + 4), disp = rd32(hd + 8), funclet = rd32(hd + 12);
                uint32_t ct = 0;
                if (ptype) {
                    for (uint32_t i = 0; i < ntypes && !ct; i++) {
                        uint32_t c = rd32(cta + 4 + 4 * i), td = rd32(c + 4);
                        if (td == ptype || !strcmp(type_name(td), type_name(ptype))) ct = c;
                    }
                    if (!ct) continue;
                }
                /* found: the anchor of this function's call */
                eh_anchor *a = t_top;
                while (a && !(a->esp > rn)) a = a->up;
                if (!a || funcinfo_of_fn(a->va) != fi) {
                    TM_WARN("eh: %s from %08X is caught by FuncInfo %08X (frame %08X), but that call has no anchor (%s %08X)",
                            tname, from, fi, rn, a ? "innermost outer call is" : "none", a ? a->va : 0);
                    return;
                }
                if (logged++ < 50) {
                    char what[160] = "";
                    if (strstr(tname, "CNetSystemError")) {          /* { int, CFastString, int?, CFastString } */
                        uint32_t p1 = rd32(obj + 4), l1 = rd32(obj + 8), p2 = rd32(obj + 0x14), l2 = rd32(obj + 0x18);
                        if (p1 && l1 && l1 < 70) snprintf(what, sizeof what, ": %.*s", (int)l1, (const char *)G2H(p1));
                        if (p2 && l2 && l2 < 70) snprintf(what + strlen(what), sizeof what - strlen(what), " / %.*s", (int)l2, (const char *)G2H(p2));
                    }
                    char trail[96] = ""; int k = 0, m = 0;          /* likely return addresses on the guest stack */
                    for (uint32_t o = 0; o < 0x300 && m < 6; o += 4) {
                        uint32_t v = rd32(g_esp + o);
                        if (v > 0x00401000u && v < 0x016A3000u) { k += snprintf(trail + k, sizeof trail - k, " %08X", v); m++; }
                    }
                    TM_INFO("eh: %s%s thrown at %08X, caught in %08X (try %u, state %d, handler %08X), stack:%s", tname, what, from,
                            a->va, t, st, funclet, trail);
                }
                /* leave the inner frames: their destructors, innermost first */
                for (uint32_t r = rd32(teb); r != rn && r && r != 0xFFFFFFFFu; r = rd32(r)) {
                    uint32_t f2 = thunk_funcinfo(rd32(r + 4));
                    if (f2) unwind_to(r, f2, -1);
                    wr32(teb, rd32(r));
                }
                wr32(teb, rn);
                unwind_to(rn, fi, lo);                                /* the try block's own objects */
                wr32(rn + 8, (uint32_t)(hi + 1));
                if (ptype && disp) {                                  /* the catch object */
                    uint32_t dst = rn + 0xC + disp, src = obj + rd32(ct + 8);
                    if (adj & 8) wr32(dst, src);                      /* by reference */
                    else if ((rd32(ct) & 1) || !rd32(ct + 24)) memmove(G2H(dst), G2H(src), rd32(ct + 20));
                    else tm_call_guest_this(rd32(ct + 24), dst, 1, src);
                }
                uint32_t prev_obj = t_cur_obj, prev_ti = t_cur_ti;
                t_cur_obj = obj; t_cur_ti = ti;
                uint32_t cont = call_funclet(funclet, rn + 0xC);
                t_cur_obj = prev_obj; t_cur_ti = prev_ti;
                if (rd32(ti + 4)) tm_call_guest_this(rd32(ti + 4), obj, 0);   /* destroy the exception object */
                a->cont = cont; a->frame = rn + 0xC;
                t_top = a;
                longjmp(a->jb, 1);
            }
        }
    }
    TM_WARN("eh: no handler for %s thrown at %08X", tname, from);
}
