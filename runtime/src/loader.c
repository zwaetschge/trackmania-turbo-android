/*
 * loader.c - map the original PE32 image at its preferred base inside the
 * guest space, bind its IAT to synthetic shim addresses, and set up static
 * TLS. The EXE bytes on disk are never modified.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

tm_image g_image;

/* ------------------------------------------------ shim registry */
typedef struct {
    char key[160];        /* "dll!name", dll lower-case */
    char dll[64];
    tm_shim_fn fn;        /* NULL = missing (strict stop when called) */
} shim_rec;

static shim_rec *g_shims;          /* index = (va - TM_SHIM_BASE) / STRIDE */
static uint32_t g_nshims;
static int32_t *g_shim_hash;       /* open addressing, -1 = empty */
#define HASH_SIZE (1u << 16)
static pthread_mutex_t g_shim_mu = PTHREAD_MUTEX_INITIALIZER;

static char g_known_dlls[256][64];
static int g_nknown;

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void norm_dll(const char *dll, char *out, size_t cap) {
    size_t i = 0;
    for (; dll[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)dll[i]);
    out[i] = 0;
    if (!strchr(out, '.') && i + 4 < cap) strcat(out, ".dll");
}

static void shims_init(void) {
    if (g_shims) return;
    g_shims = calloc(TM_SHIM_MAX, sizeof *g_shims);
    g_shim_hash = malloc(HASH_SIZE * sizeof(int32_t));
    memset(g_shim_hash, 0xFF, HASH_SIZE * sizeof(int32_t));
}

static int shim_find(const char *key) {
    uint32_t h = fnv(key) & (HASH_SIZE - 1);
    for (;;) {
        int32_t i = g_shim_hash[h];
        if (i < 0) return -1;
        if (!strcmp(g_shims[i].key, key)) return i;
        h = (h + 1) & (HASH_SIZE - 1);
    }
}

static int shim_add(const char *dll, const char *name, tm_shim_fn fn) {
    if (g_nshims >= TM_SHIM_MAX - 1) tm_fatal("shim table full");
    shim_rec *r = &g_shims[g_nshims];
    snprintf(r->key, sizeof r->key, "%s!%s", dll, name);
    snprintf(r->dll, sizeof r->dll, "%s", dll);
    r->fn = fn;
    uint32_t h = fnv(r->key) & (HASH_SIZE - 1);
    while (g_shim_hash[h] >= 0) h = (h + 1) & (HASH_SIZE - 1);
    g_shim_hash[h] = (int32_t)g_nshims;
    return (int)g_nshims++;
}

static void known_add(const char *dll) {
    for (int i = 0; i < g_nknown; i++) if (!strcmp(g_known_dlls[i], dll)) return;
    if (g_nknown < 256) snprintf(g_known_dlls[g_nknown++], 64, "%s", dll);
}

void tm_register_shims(const tm_shim_def *defs, size_t count) {
    pthread_mutex_lock(&g_shim_mu);
    shims_init();
    for (size_t i = 0; i < count; i++) {
        char dll[64], key[160];
        norm_dll(defs[i].dll, dll, sizeof dll);
        snprintf(key, sizeof key, "%s!%s", dll, defs[i].name);
        int k = shim_find(key);
        if (k >= 0) g_shims[k].fn = defs[i].fn;
        else shim_add(dll, defs[i].name, defs[i].fn);
        known_add(dll);
    }
    pthread_mutex_unlock(&g_shim_mu);
}

uint32_t tm_import_va(const char *dll_in, const char *name, int create_missing) {
    char dll[64], key[160];
    norm_dll(dll_in, dll, sizeof dll);
    snprintf(key, sizeof key, "%s!%s", dll, name);
    pthread_mutex_lock(&g_shim_mu);
    shims_init();
    int k = shim_find(key);
    if (k < 0 && create_missing) k = shim_add(dll, name, NULL);
    pthread_mutex_unlock(&g_shim_mu);
    if (k < 0) return 0;
    return TM_SHIM_BASE + (uint32_t)k * TM_SHIM_STRIDE;
}

uint32_t tm_proc_address(const char *dll_in, const char *name) {
    char dll[64], key[160];
    norm_dll(dll_in, dll, sizeof dll);
    snprintf(key, sizeof key, "%s!%s", dll, name);
    pthread_mutex_lock(&g_shim_mu);
    shims_init();
    int k = shim_find(key);
    pthread_mutex_unlock(&g_shim_mu);
    if (k < 0 || !g_shims[k].fn) return 0;
    return TM_SHIM_BASE + (uint32_t)k * TM_SHIM_STRIDE;
}

const char *tm_import_name(uint32_t va) {
    if (va >= TM_SHIM_BASE && va < TM_SHIM_BASE + g_nshims * TM_SHIM_STRIDE && (va - TM_SHIM_BASE) % TM_SHIM_STRIDE == 0)
        return g_shims[(va - TM_SHIM_BASE) / TM_SHIM_STRIDE].key;
    if (va >= g_image.iat_lo && va < g_image.iat_hi) return tm_import_name(rd32(va));
    return NULL;
}

int tm_module_known(const char *dll_in) {
    char dll[64];
    norm_dll(dll_in, dll, sizeof dll);
    for (int i = 0; i < g_nknown; i++) if (!strcmp(g_known_dlls[i], dll)) return 1;
    return 0;
}

uint32_t tm_module_handle(const char *dll_in) {
    char dll[64];
    norm_dll(dll_in, dll, sizeof dll);
    for (int i = 0; i < g_nknown; i++) if (!strcmp(g_known_dlls[i], dll)) return TM_HMOD_BASE + (uint32_t)i * 0x10000u;
    return 0;
}

const char *tm_module_name(uint32_t h) {
    if (h < TM_HMOD_BASE) return NULL;
    uint32_t i = (h - TM_HMOD_BASE) / 0x10000u;
    return (int)i < g_nknown && (h & 0xFFFF) == 0 ? g_known_dlls[i] : NULL;
}

/* Called for an import nobody implemented: stop with the caller named. */
static __thread uint32_t t_missing_va;
static void missing_import(void) {
    tm_fatal("unimplemented import %s called from guest function %08X (args %08X %08X %08X %08X)",
             tm_import_name(t_missing_va), g_cur_func, ARG(0), ARG(1), ARG(2), ARG(3));
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    if (va >= g_image.iat_lo && va < g_image.iat_hi) va = rd32(va);
    if (va >= TM_SHIM_BASE && va < TM_SHIM_BASE + g_nshims * TM_SHIM_STRIDE && (va - TM_SHIM_BASE) % TM_SHIM_STRIDE == 0) {
        shim_rec *r = &g_shims[(va - TM_SHIM_BASE) / TM_SHIM_STRIDE];
        if (r->fn) {
            if (tm_log_level >= TM_LOG_TRACE) tm_log(TM_LOG_TRACE, "-> %s from %08X", r->key, g_cur_func);
            return r->fn;
        }
        t_missing_va = va;
        return missing_import;
    }
    return NULL;
}

/* ------------------------------------------------ PE loading */
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int tm_load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { TM_ERR("cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(file); TM_ERR("read failed"); return -1; }
    fclose(f);
    if (fsz < 0x400 || file[0] != 'M' || file[1] != 'Z') { free(file); TM_ERR("not an MZ image"); return -1; }
    uint32_t pe = le32(file + 0x3C);
    if (le32(file + pe) != 0x00004550u || le16(file + pe + 4) != 0x14C) { free(file); TM_ERR("not a PE32 x86 image"); return -1; }
    uint16_t nsec = le16(file + pe + 6), optsz = le16(file + pe + 20);
    const uint8_t *opt = file + pe + 24;
    if (le16(opt) != 0x10B) { free(file); TM_ERR("not PE32"); return -1; }
    g_image.entry = le32(opt + 16);
    g_image.image_base = le32(opt + 28);
    g_image.size_of_image = le32(opt + 56);
    uint32_t size_hdr = le32(opt + 60);
    const uint8_t *dd = opt + 96;
    uint32_t imp_rva = le32(dd + 8), rsrc_rva = le32(dd + 16), rsrc_sz = le32(dd + 20);
    uint32_t tls_rva = le32(dd + 9 * 8), iat_rva = le32(dd + 12 * 8), iat_sz = le32(dd + 12 * 8 + 4);
    g_image.entry += g_image.image_base;
    uint32_t base = g_image.image_base;
    if (!tm_vm_alloc(base, g_image.size_of_image, 0x3000, 4)) { free(file); TM_ERR("image range %08X busy", base); return -1; }
    tm_vm_mark_image(base, g_image.size_of_image);
    memcpy(G2H(base), file, size_hdr);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsz = le32(sh + 8), va = le32(sh + 12), rsz = le32(sh + 16), roff = le32(sh + 20);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        if (roff + n > (uint32_t)fsz) n = (uint32_t)fsz - roff;
        memcpy(G2H(base + va), file + roff, n);
        char nm[9] = {0}; memcpy(nm, sh, 8);
        TM_INFO("section %-8s %08X vsize %08X raw %08X", nm, base + va, vsz, rsz);
    }
    free(file);
    if (tls_rva) {
        uint32_t t = base + tls_rva;
        g_image.tls_dir = t;
        g_image.tls_tmpl_start = rd32(t + 0);
        g_image.tls_tmpl_end = rd32(t + 4);
        g_image.tls_index_va = rd32(t + 8);
        g_image.tls_callbacks = rd32(t + 12);
        g_image.tls_zero_fill = rd32(t + 16);
        wr32(g_image.tls_index_va, 0);
        TM_INFO("static TLS template %08X-%08X zero %u callbacks %08X", g_image.tls_tmpl_start, g_image.tls_tmpl_end,
                g_image.tls_zero_fill, g_image.tls_callbacks);
    }
    g_image.rsrc_va = rsrc_rva ? base + rsrc_rva : 0;
    g_image.rsrc_size = rsrc_sz;
    g_image.iat_lo = iat_rva ? base + iat_rva : 0;
    g_image.iat_hi = iat_rva ? base + iat_rva + iat_sz : 0;
    /* imports are bound later (shims must be registered first) */
    wr32(base + 0x3C + 0, rd32(base + 0x3C)); /* keep headers intact */
    (void)imp_rva;
    snprintf(g_image.path, sizeof g_image.path, "%s", path);
    snprintf(g_image.dir, sizeof g_image.dir, "%s", path);
    char *slash = strrchr(g_image.dir, '/');
    if (slash) *slash = 0;
    TM_INFO("image %s base %08X size %08X entry %08X", path, base, g_image.size_of_image, g_image.entry);
    return 0;
}

/* Write synthetic shim VAs into the import address table of the image at base. */
static int bind_imports_at(uint32_t base, uint32_t *lo_out, uint32_t *hi_out, int *missing_out) {
    uint32_t pe = base + rd32(base + 0x3C);
    uint32_t opt = pe + 24;
    uint32_t imp = rd32(opt + 96 + 8);
    int bound = 0, missing = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    if (imp) for (uint32_t d = base + imp; rd32(d + 12); d += 20) {
        const char *dll = (const char *)G2H(base + rd32(d + 12));
        uint32_t ilt = rd32(d) ? base + rd32(d) : base + rd32(d + 16);
        uint32_t iat = base + rd32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t ent = rd32(ilt + 4 * k);
            if (!ent) break;
            char name[128];
            if (ent & 0x80000000u) snprintf(name, sizeof name, "#%u", ent & 0xFFFF);
            else snprintf(name, sizeof name, "%s", (const char *)G2H(base + ent + 2));
            uint32_t va = tm_import_va(dll, name, 1);
            if (!tm_proc_address(dll, name)) missing++;
            wr32(iat + 4 * k, va);
            if (iat + 4 * k < lo) lo = iat + 4 * k;
            if (iat + 4 * k + 4 > hi) hi = iat + 4 * k + 4;
            bound++;
        }
    }
    if (lo_out) *lo_out = lo;
    if (hi_out) *hi_out = hi;
    if (missing_out) *missing_out = missing;
    return bound;
}

int tm_bind_imports(void) {
    uint32_t lo, hi; int missing;
    int bound = bind_imports_at(g_image.image_base, &lo, &hi, &missing);
    if (bound) {
        if (!g_image.iat_lo || lo < g_image.iat_lo) g_image.iat_lo = lo;
        if (hi > g_image.iat_hi) g_image.iat_hi = hi;
    }
    TM_INFO("bound %d imports (%d without implementation; they stop when called)", bound, missing);
    return missing;
}

/* ------------------------------------------------ lifted bundled DLLs */
/* Map one DLL from the game directory at the base it was lifted for. Returns
 * its entry point VA (0 if none), or 0xFFFFFFFF on failure. */
static uint32_t load_module(const tm_lifted_module *m) {
    char path[1300];
    snprintf(path, sizeof path, "%s/%s", g_image.dir, m->dll);
    FILE *f = fopen(path, "rb");
    if (!f) { TM_WARN("lifted module %s: file not found in the game directory", m->dll); return 0xFFFFFFFFu; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(file); return 0xFFFFFFFFu; }
    fclose(f);
    uint32_t pe = fsz >= 0x400 && file[0] == 'M' && file[1] == 'Z' ? le32(file + 0x3C) : 0;
    if (!pe || pe + 0x100 > (uint32_t)fsz || le32(file + pe) != 0x00004550u || le16(file + pe + 4) != 0x14C) {
        free(file); TM_WARN("lifted module %s: not a PE32 x86 image", m->dll); return 0xFFFFFFFFu;
    }
    uint16_t nsec = le16(file + pe + 6), optsz = le16(file + pe + 20);
    const uint8_t *opt = file + pe + 24;
    uint32_t entry = le32(opt + 16), pref = le32(opt + 28), size = le32(opt + 56), size_hdr = le32(opt + 60);
    const uint8_t *dd = opt + 96;
    uint32_t exp_rva = le32(dd + 0), rel_rva = le32(dd + 5 * 8), rel_sz = le32(dd + 5 * 8 + 4);
    uint32_t base = m->base;
    if (!tm_vm_alloc(base, size, 0x3000, 4)) { free(file); TM_ERR("lifted module %s: range %08X busy", m->dll, base); return 0xFFFFFFFFu; }
    tm_vm_mark_image(base, size);
    memcpy(G2H(base), file, size_hdr);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsz = le32(sh + 8), va = le32(sh + 12), rsz = le32(sh + 16), roff = le32(sh + 20);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        if (roff + n > (uint32_t)fsz) n = (uint32_t)fsz - roff;
        memcpy(G2H(base + va), file + roff, n);
    }
    free(file);
    /* base relocations, when the lifted base is not the DLL's preferred one */
    uint32_t delta = base - pref, nrel = 0;
    if (delta && rel_rva) {
        for (uint32_t b = base + rel_rva, end = b + rel_sz; b + 8 <= end;) {
            uint32_t page = rd32(b), bsz = rd32(b + 4);
            if (bsz < 8) break;
            for (uint32_t e = b + 8; e + 2 <= b + bsz; e += 2) {
                uint16_t w = rd16(e);
                if ((w >> 12) == 3) { uint32_t a = base + page + (w & 0xFFF); wr32(a, rd32(a) + delta); nrel++; }
            }
            b += bsz;
        }
    }
    int missing = 0;
    int bound = bind_imports_at(base, NULL, NULL, &missing);
    /* exports of the DLL = the lifted functions behind the main image's imports */
    int nexp = 0, nmiss = 0;
    if (exp_rva) {
        uint32_t ed = base + exp_rva;
        uint32_t nnames = rd32(ed + 24), funcs = base + rd32(ed + 28), names = base + rd32(ed + 32), ords = base + rd32(ed + 36);
        for (uint32_t i = 0; i < nnames; i++) {
            const char *nm = (const char *)G2H(base + rd32(names + 4 * i));
            uint32_t fva = base + rd32(funcs + 4u * rd16(ords + 2 * i));
            recomp_func_t fn = recomp_lookup(fva);
            if (!fn) { nmiss++; TM_WARN("lifted module %s: export %s at %08X was not lifted", m->dll, nm, fva); continue; }
            fn = tm_parallel_wrap_export(m->dll, nm, fn);     /* a few exports run outside the machine lock */
            tm_shim_def d = { m->dll, strdup(nm), fn };
            tm_register_shims(&d, 1);
            nexp++;
        }
    }
    TM_INFO("lifted module %s at %08X (%u relocations, %d imports bound, %d missing, %d exports, %d not lifted)",
            m->dll, base, nrel, bound, missing, nexp, nmiss);
    return entry ? base + entry : 0;
}

void tm_load_lifted_modules(void) {
    for (const tm_lifted_module *m = tm_lifted_modules; m->dll; m++) {
        if (!m->count || !m->table) continue;          /* not linked into this build */
        uint32_t entry = load_module(m);
        if (entry == 0xFFFFFFFFu || !entry) continue;
        /* DllMain(hinst, DLL_PROCESS_ATTACH, NULL): the DLL's own CRT start-up */
        uint32_t ok = tm_call_guest(entry, 3, m->base, 1, 0);
        if (!ok) TM_WARN("lifted module %s: DllMain returned FALSE", m->dll);
    }
}

/* ------------------------------------------------ static TLS */
int tm_tls_thread_init(uint32_t teb) {
    if (!g_image.tls_dir) return 0;
    uint32_t tsize = g_image.tls_tmpl_end - g_image.tls_tmpl_start;
    uint32_t total = tsize + g_image.tls_zero_fill;
    uint32_t block = tm_galloc(total + 16);
    if (!block) return -1;
    memcpy(G2H(block), G2H(g_image.tls_tmpl_start), tsize);
    memset(G2H(block + tsize), 0, g_image.tls_zero_fill);
    uint32_t arr = tm_galloc(64 * 4);
    wr32(arr, block);
    wr32(teb + 0x2C, arr);
    return 0;
}

void tm_tls_callbacks(uint32_t reason) {
    if (!g_image.tls_callbacks) return;
    for (uint32_t p = g_image.tls_callbacks; rd32(p); p += 4) {
        uint32_t cb = rd32(p);
        TM_INFO("TLS callback %08X reason %u", cb, reason);
        tm_call_guest(cb, 3, g_image.image_base, reason, 0);
    }
}
