/*
 * mem.c - guest virtual memory (VirtualAlloc & co.) and guest heaps.
 *
 * The full 4 GB guest space is reserved PROT_NONE once. Guest pages are 4 KB;
 * host pages may be 4 KB or 16 KB, so host protection is only ever widened to
 * RW for committed ranges (x86 page protection is guest metadata, ARM64 never
 * executes guest bytes). Bookkeeping is a flat per-guest-page table.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <sys/mman.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GPAGE 4096u
#define NPAGES (1u << 20)
#define GRAN  0x10000u

enum { PS_FREE = 0, PS_RESERVED = 1, PS_COMMITTED = 2 };
static uint8_t  *pg_state;
static uint8_t  *pg_prot;     /* Windows PAGE_* low byte */
static uint32_t *pg_base;     /* AllocationBase */
static uint8_t  *pg_type;     /* 1 = private, 2 = image */
static uintptr_t host_page;
static pthread_mutex_t vm_mu = PTHREAD_MUTEX_INITIALIZER;

int tm_mem_init(void) {
    size_t total = ((size_t)1 << 32) + 0x10000;
    void *p = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { TM_ERR("guest address space reservation failed"); return -1; }
    g_mem_base = (ptrdiff_t)(uintptr_t)p;
    host_page = (uintptr_t)sysconf(_SC_PAGESIZE);
    pg_state = calloc(NPAGES, 1);
    pg_prot = calloc(NPAGES, 1);
    pg_type = calloc(NPAGES, 1);
    pg_base = calloc(NPAGES, 4);
    if (!pg_state || !pg_prot || !pg_base || !pg_type) return -1;
    TM_INFO("guest space reserved at host %p (host page %lu)", p, (unsigned long)host_page);
    return 0;
}

/* Make host pages covering [va, va+size) readable+writable. */
int tm_mem_commit(uint32_t va, uint32_t size) {
    if (!size) return 0;
    uintptr_t s = (uintptr_t)g_mem_base + va;
    uintptr_t e = s + size;
    s &= ~(host_page - 1);
    e = (e + host_page - 1) & ~(host_page - 1);
    if (mprotect((void *)s, e - s, PROT_READ | PROT_WRITE) != 0) {
        TM_ERR("mprotect commit %08X+%X failed", va, size);
        return -1;
    }
    return 0;
}

static int range_state_is(uint32_t first, uint32_t n, int st) {
    for (uint32_t i = 0; i < n; i++) if (pg_state[first + i] != st) return 0;
    return 1;
}

static uint32_t find_free(uint32_t size, int top_down) {
    uint32_t npg = (size + GPAGE - 1) / GPAGE;
    if (!top_down) {
        for (uint32_t a = TM_ALLOC_LO; a + size <= TM_ALLOC_HI && a + size > a; a += GRAN)
            if (range_state_is(a / GPAGE, npg, PS_FREE)) return a;
    } else {
        for (uint32_t a = (TM_ALLOC_HI - size) & ~(GRAN - 1); a >= TM_ALLOC_LO; a -= GRAN) {
            if (range_state_is(a / GPAGE, npg, PS_FREE)) return a;
            if (a < GRAN) break;
        }
    }
    return 0;
}

/* Rolling hint for the common bottom-up case so successive allocations do not
 * rescan the whole used prefix. */
static uint32_t g_alloc_hint = TM_ALLOC_LO;

static uint32_t find_free_hint(uint32_t size) {
    uint32_t npg = (size + GPAGE - 1) / GPAGE;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? TM_ALLOC_LO : g_alloc_hint;
        for (uint32_t a = start; a + size <= TM_ALLOC_HI && a + size > a; a += GRAN) {
            if (range_state_is(a / GPAGE, npg, PS_FREE)) {
                g_alloc_hint = (a + size + GRAN - 1) & ~(GRAN - 1);
                return a;
            }
        }
    }
    return 0;
}

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u
#define MEM_RESET    0x80000u
#define MEM_TOP_DOWN 0x100000u

uint32_t tm_vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot) {
    if (!size) return 0;
    pthread_mutex_lock(&vm_mu);
    uint32_t result = 0;
    if (type & MEM_RESET) { result = addr; goto out; }
    if (!(type & (MEM_COMMIT | MEM_RESERVE))) goto out;
    if (type & MEM_RESERVE || !addr) {
        uint32_t base, len;
        if (addr) {
            base = addr & ~(GRAN - 1);
            len = ((addr + size + GPAGE - 1) & ~(GPAGE - 1)) - base;
            if (!range_state_is(base / GPAGE, len / GPAGE, PS_FREE)) goto out;
        } else {
            len = (size + GPAGE - 1) & ~(GPAGE - 1);
            base = (type & MEM_TOP_DOWN) ? find_free(len, 1) : find_free_hint(len);
            if (!base) { TM_ERR("VirtualAlloc: out of guest address space for %u bytes", size); goto out; }
        }
        for (uint32_t i = 0; i < len / GPAGE; i++) {
            uint32_t p = base / GPAGE + i;
            pg_state[p] = PS_RESERVED; pg_base[p] = base; pg_prot[p] = (uint8_t)prot; pg_type[p] = 1;
        }
        addr = addr ? addr : base;
        if (!(type & MEM_COMMIT)) { result = base; goto out; }
        size = len - (addr - base);
        result = base;
    }
    if (type & MEM_COMMIT) {
        uint32_t s = addr & ~(GPAGE - 1);
        uint32_t e = (addr + size + GPAGE - 1) & ~(GPAGE - 1);
        for (uint32_t p = s / GPAGE; p < e / GPAGE; p++)
            if (pg_state[p] == PS_FREE) { TM_WARN("VirtualAlloc commit of unreserved %08X", p * GPAGE); goto out; }
        if (tm_mem_commit(s, e - s) != 0) goto out;
        for (uint32_t p = s / GPAGE; p < e / GPAGE; p++) {
            if (pg_state[p] != PS_COMMITTED) {
                /* re-commit of decommitted pages must read as zero */
                if (pg_state[p] == PS_RESERVED && pg_type[p] == 3) memset(G2H(p * GPAGE), 0, GPAGE);
                pg_state[p] = PS_COMMITTED;
            }
            pg_prot[p] = (uint8_t)prot;
            if (pg_type[p] == 3) pg_type[p] = 1;
        }
        if (!result) result = s;
    }
out:
    pthread_mutex_unlock(&vm_mu);
    return result;
}

int tm_vm_free(uint32_t addr, uint32_t size, uint32_t type) {
    pthread_mutex_lock(&vm_mu);
    int ok = 0;
    uint32_t p0 = addr / GPAGE;
    if (type & MEM_RELEASE) {
        if (size != 0 || pg_state[p0] == PS_FREE || pg_base[p0] != addr) goto out;
        uint32_t p = p0;
        while (p < NPAGES && pg_state[p] != PS_FREE && pg_base[p] == addr) {
            if (pg_state[p] == PS_COMMITTED) memset(G2H(p * GPAGE), 0, GPAGE);
            pg_state[p] = PS_FREE; pg_base[p] = 0; pg_prot[p] = 0; pg_type[p] = 0;
            p++;
        }
        uint32_t len = (p - p0) * GPAGE;
        /* Return whole host pages to the kernel. */
        uintptr_t hs = ((uintptr_t)g_mem_base + addr + host_page - 1) & ~(host_page - 1);
        uintptr_t he = ((uintptr_t)g_mem_base + addr + len) & ~(host_page - 1);
        if (he > hs) madvise((void *)hs, he - hs, MADV_DONTNEED);
        if (addr < g_alloc_hint) g_alloc_hint = addr & ~(GRAN - 1);
        ok = 1;
    } else if (type & MEM_DECOMMIT) {
        uint32_t e = size ? (addr + size + GPAGE - 1) / GPAGE : NPAGES;
        for (uint32_t p = p0; p < e && pg_state[p] != PS_FREE; p++) {
            if (!size && pg_base[p] != pg_base[p0]) break;
            if (pg_state[p] == PS_COMMITTED) { memset(G2H(p * GPAGE), 0, GPAGE); pg_state[p] = PS_RESERVED; pg_type[p] = 3; }
        }
        ok = 1;
    }
out:
    pthread_mutex_unlock(&vm_mu);
    return ok;
}

int tm_vm_query(uint32_t addr, uint32_t mbi) {
    pthread_mutex_lock(&vm_mu);
    uint32_t p = addr / GPAGE;
    uint8_t st = pg_state[p];
    uint32_t base = pg_base[p];
    uint8_t prot = pg_prot[p];
    uint32_t q = p;
    while (q < NPAGES && pg_state[q] == st && pg_base[q] == base && (st != PS_COMMITTED || pg_prot[q] == prot)) q++;
    pthread_mutex_unlock(&vm_mu);
    wr32(mbi + 0, p * GPAGE);                                  /* BaseAddress */
    wr32(mbi + 4, st == PS_FREE ? 0 : base);                   /* AllocationBase */
    wr32(mbi + 8, st == PS_FREE ? 0 : 4);                      /* AllocationProtect */
    wr32(mbi + 12, (q - p) * GPAGE);                           /* RegionSize */
    wr32(mbi + 16, st == PS_FREE ? 0x10000 : st == PS_RESERVED ? 0x2000 : 0x1000); /* State */
    wr32(mbi + 20, st == PS_COMMITTED ? prot : 0);             /* Protect */
    wr32(mbi + 24, st == PS_FREE ? 0 : (pg_type[p] == 2 ? 0x1000000 : 0x20000)); /* Type */
    return 28;
}

int tm_vm_protect(uint32_t addr, uint32_t size, uint32_t prot, uint32_t *old) {
    pthread_mutex_lock(&vm_mu);
    uint32_t s = addr / GPAGE, e = (addr + size + GPAGE - 1) / GPAGE;
    int ok = 1;
    for (uint32_t p = s; p < e; p++) if (pg_state[p] != PS_COMMITTED) ok = 0;
    if (ok) {
        if (old) *old = pg_prot[s];
        for (uint32_t p = s; p < e; p++) pg_prot[p] = (uint8_t)prot;
    }
    pthread_mutex_unlock(&vm_mu);
    return ok;
}

void tm_vm_mark_image(uint32_t base, uint32_t size) {
    pthread_mutex_lock(&vm_mu);
    for (uint32_t p = base / GPAGE; p < (base + size + GPAGE - 1) / GPAGE; p++) {
        pg_state[p] = PS_COMMITTED; pg_base[p] = base; pg_prot[p] = 0x04; pg_type[p] = 2;
    }
    pthread_mutex_unlock(&vm_mu);
}

/* ================================================================ heaps */
/*
 * Segregated-fit allocator inside guest memory.
 *   small : <= 1024 bytes, 16-byte classes
 *   medium: <= 1 MB, geometric classes (8 per power of two)
 *   large : direct VirtualAlloc
 * Every block carries a 16-byte header: size, class, magic, heap.
 */
#define HDR 16u
#define MAGIC_LIVE 0x48504C56u  /* "VLPH" */
#define MAGIC_FREE 0x45455246u
#define SMALL_MAX 1024u
#define MEDIUM_MAX (1u << 20)
#define NCLASS 256
#define CLASS_LARGE 0xFFFFu

typedef struct tm_heap {
    uint32_t handle;
    uint32_t free_head[NCLASS];   /* guest VA of first free block (header VA) */
    uint32_t arena_cur, arena_end;
    uint64_t live_bytes, live_blocks;
    pthread_mutex_t mu;
} tm_heap;

#define MAX_HEAPS 64
static tm_heap *g_heaps[MAX_HEAPS];
static uint32_t g_process_heap;
static pthread_mutex_t heaps_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t class_size_tab[NCLASS];
static int nclasses;

static void init_classes(void) {
    if (nclasses) return;
    int c = 0;
    for (uint32_t s = 16; s <= SMALL_MAX; s += 16) class_size_tab[c++] = s;
    uint32_t s = SMALL_MAX;
    while (s < MEDIUM_MAX) {
        uint32_t step = s / 8;
        for (int i = 0; i < 8 && s < MEDIUM_MAX; i++) { s += step; class_size_tab[c++] = (s + 15) & ~15u; }
    }
    nclasses = c;
}

static int size_class(uint32_t n) {
    if (n <= SMALL_MAX) return n ? (int)((n + 15) / 16) - 1 : 0;
    int lo = SMALL_MAX / 16, hi = nclasses - 1;
    while (lo < hi) { int mid = (lo + hi) / 2; if (class_size_tab[mid] >= n) hi = mid; else lo = mid + 1; }
    return lo;
}

static tm_heap *heap_from_handle(uint32_t h) {
    if (!h) return NULL;
    for (int i = 0; i < MAX_HEAPS; i++) if (g_heaps[i] && g_heaps[i]->handle == h) return g_heaps[i];
    return NULL;
}

uint32_t tm_heap_create(uint32_t flags) {
    (void)flags;
    init_classes();
    tm_heap *hp = calloc(1, sizeof *hp);
    pthread_mutex_init(&hp->mu, NULL);
    /* The handle is a guest address so code that peeks at it sees memory. */
    uint32_t h = tm_vm_alloc(0, GRAN, MEM_RESERVE | MEM_COMMIT, 4);
    hp->handle = h;
    pthread_mutex_lock(&heaps_mu);
    for (int i = 0; i < MAX_HEAPS; i++) if (!g_heaps[i]) { g_heaps[i] = hp; break; }
    pthread_mutex_unlock(&heaps_mu);
    return h;
}

uint32_t tm_heap_process(void) {
    if (!g_process_heap) g_process_heap = tm_heap_create(0);
    return g_process_heap;
}

int tm_heap_destroy(uint32_t h) {
    /* Memory is not returned (arenas are shared bookkeeping); the heap just
     * stops being valid. Games destroy heaps at shutdown. */
    pthread_mutex_lock(&heaps_mu);
    for (int i = 0; i < MAX_HEAPS; i++) if (g_heaps[i] && g_heaps[i]->handle == h) { g_heaps[i] = NULL; }
    pthread_mutex_unlock(&heaps_mu);
    return 1;
}

static uint32_t carve(tm_heap *hp, uint32_t total) {
    if (hp->arena_cur + total > hp->arena_end || !hp->arena_cur) {
        uint32_t chunk = total > (4u << 20) ? ((total + GRAN - 1) & ~(GRAN - 1)) : (4u << 20);
        uint32_t a = tm_vm_alloc(0, chunk, MEM_RESERVE | MEM_COMMIT, 4);
        if (!a) return 0;
        hp->arena_cur = a; hp->arena_end = a + chunk;
    }
    uint32_t r = hp->arena_cur;
    hp->arena_cur += total;
    return r;
}

uint32_t tm_heap_alloc(uint32_t h, uint32_t flags, uint32_t size) {
    tm_heap *hp = heap_from_handle(h);
    if (!hp) hp = heap_from_handle(tm_heap_process());
    uint32_t hdr;
    pthread_mutex_lock(&hp->mu);
    if (size > MEDIUM_MAX - HDR) {
        uint32_t total = (size + HDR + GPAGE - 1) & ~(GPAGE - 1);
        hdr = tm_vm_alloc(0, total, MEM_RESERVE | MEM_COMMIT, 4);
        if (!hdr) { pthread_mutex_unlock(&hp->mu); return 0; }
        wr32(hdr + 4, CLASS_LARGE);
    } else {
        int c = size_class(size);
        hdr = hp->free_head[c];
        if (hdr) {
            hp->free_head[c] = rd32(hdr + HDR);
            if (rd32(hdr + 8) != MAGIC_FREE) tm_fatal("heap corruption: free block %08X has bad magic", hdr);
            if (flags & 0x8) memset(G2H(hdr + HDR), 0, class_size_tab[c]);
            else wr32(hdr + HDR, 0);
        } else {
            hdr = carve(hp, class_size_tab[c] + HDR);
            if (!hdr) { pthread_mutex_unlock(&hp->mu); return 0; }
        }
        wr32(hdr + 4, (uint32_t)c);
    }
    wr32(hdr + 0, size);
    wr32(hdr + 8, MAGIC_LIVE);
    wr32(hdr + 12, hp->handle);
    hp->live_bytes += size; hp->live_blocks++;
    pthread_mutex_unlock(&hp->mu);
    static int zeroall = -1;
    if (zeroall < 0) zeroall = getenv("TM_HEAP_ZERO") ? 1 : 0;
    if (zeroall && !(flags & 0x8)) memset(G2H(hdr + HDR), 0, size);
    return hdr + HDR;
}

static int block_ok(uint32_t p) {
    return p > HDR && (p & 7) == 0 && rd32(p - HDR + 8) == MAGIC_LIVE;
}

int tm_heap_free(uint32_t h, uint32_t flags, uint32_t p) {
    (void)flags;
    if (!p) return 1;
    if (!block_ok(p)) { TM_WARN("HeapFree of invalid block %08X (heap %08X) from %08X", p, h, g_cur_func); return 0; }
    uint32_t hdr = p - HDR;
    tm_heap *hp = heap_from_handle(rd32(hdr + 12));
    if (!hp) hp = heap_from_handle(tm_heap_process());
    pthread_mutex_lock(&hp->mu);
    uint32_t c = rd32(hdr + 4);
    hp->live_bytes -= rd32(hdr); hp->live_blocks--;
    if (c == CLASS_LARGE) {
        wr32(hdr + 8, 0);
        tm_vm_free(hdr, 0, MEM_RELEASE);
    } else {
        static int noreuse = -1;
        if (noreuse < 0) noreuse = getenv("TM_HEAP_NOREUSE") ? 1 : 0;
        if (!noreuse) {
            wr32(hdr + 8, MAGIC_FREE);
            wr32(hdr + HDR, hp->free_head[c]);
            hp->free_head[c] = hdr;
        }
    }
    pthread_mutex_unlock(&hp->mu);
    return 1;
}

uint32_t tm_heap_size(uint32_t h, uint32_t flags, uint32_t p) {
    (void)h; (void)flags;
    if (!block_ok(p)) return 0xFFFFFFFFu;
    return rd32(p - HDR);
}

uint32_t tm_heap_realloc(uint32_t h, uint32_t flags, uint32_t p, uint32_t size) {
    if (!p) return tm_heap_alloc(h, flags, size);
    if (!block_ok(p)) { TM_WARN("HeapReAlloc of invalid block %08X", p); return 0; }
    uint32_t hdr = p - HDR;
    uint32_t old = rd32(hdr), c = rd32(hdr + 4);
    uint32_t cap = c == CLASS_LARGE ? 0 : class_size_tab[c];
    if (c != CLASS_LARGE && size <= cap && size > cap / 2) {
        if ((flags & 0x8) && size > old) memset(G2H(p + old), 0, size - old);
        wr32(hdr, size);
        return p;
    }
    if (flags & 0x10) return 0; /* HEAP_REALLOC_IN_PLACE_ONLY */
    uint32_t n = tm_heap_alloc(rd32(hdr + 12), flags, size);
    if (!n) return 0;
    memcpy(G2H(n), G2H(p), old < size ? old : size);
    if ((flags & 0x8) && size > old) memset(G2H(n + old), 0, size - old);
    tm_heap_free(h, 0, p);
    return n;
}

uint32_t tm_galloc(uint32_t size) { return tm_heap_alloc(tm_heap_process(), 8, size); }
void tm_gfree(uint32_t va) { tm_heap_free(tm_heap_process(), 0, va); }
uint32_t tm_gstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t v = tm_galloc((uint32_t)n);
    memcpy(G2H(v), s, n);
    return v;
}
uint32_t tm_gwstrdup_utf8(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t v = tm_galloc((uint32_t)n * 2 + 2);
    tm_u2w(s, v, (int)n + 1);
    return v;
}

/* ---- diagnostics: guest memory dump (F11) ----------------------------------
 * Writes every committed, non-zero guest page to <dir>/memdump-<n>.bin plus an index
 * memdump-<n>.idx ("va len fileoffset" per contiguous run). tools/memgrep.py searches
 * it. Pages are read through /proc/self/mem so a concurrent decommit cannot fault. */
#include <fcntl.h>
static int memdump_busy;
static void *memdump_thread(void *arg) {
    (void)arg;
    const char *dir = getenv("TM_MEMDUMP_DIR");
    if (!dir || !*dir) dir = getenv("DXVK_STATE_CACHE_PATH");
    if (!dir || !*dir) dir = "work";
    static int serial;
    int n = ++serial;                               /* memdump-1, memdump-2, ... per run */
    char p[700];
    snprintf(p, sizeof p, "%s/memdump-%d.bin", dir, n);
    FILE *bin = fopen(p, "wb");
    snprintf(p, sizeof p, "%s/memdump-%d.idx.tmp", dir, n);
    FILE *idx = fopen(p, "w");
    int mem = open("/proc/self/mem", O_RDONLY);
    if (!bin || !idx) { TM_ERR("memdump: cannot write to %s", dir); if (bin) fclose(bin); if (idx) fclose(idx); if (mem >= 0) close(mem); return NULL; }
    static uint8_t page[4096], zero[4096];
    uint64_t off = 0, run_va = 0, run_len = 0, total = 0;
    for (uint32_t pg = 0x10; pg < NPAGES; pg++) {
        int ok = 0;
        if (pg_state[pg] == PS_COMMITTED) {
            uintptr_t host = (uintptr_t)g_mem_base + ((uintptr_t)pg << 12);
            ssize_t r = mem >= 0 ? pread(mem, page, 4096, (off_t)host) : -1;
            ok = r == 4096 && memcmp(page, zero, 4096) != 0;
        }
        if (ok) {
            if (!run_len) run_va = (uint64_t)pg << 12;
            fwrite(page, 1, 4096, bin);
            run_len += 4096; total += 4096;
        } else if (run_len) {
            fprintf(idx, "%08llX %llX %llX\n", (unsigned long long)run_va, (unsigned long long)run_len, (unsigned long long)off);
            off += run_len; run_len = 0;
        }
    }
    if (run_len) fprintf(idx, "%08llX %llX %llX\n", (unsigned long long)run_va, (unsigned long long)run_len, (unsigned long long)off);
    fclose(bin); fclose(idx);
    if (mem >= 0) close(mem);
    char q[700];
    snprintf(q, sizeof q, "%s/memdump-%d.idx", dir, n);
    rename(p, q);                                   /* the index appears when the dump is complete */
    TM_INFO("memdump %d: %llu MB written to %s", n, (unsigned long long)(total >> 20), dir);
    __sync_lock_release(&memdump_busy);
    return NULL;
}
void tm_memdump_request(void) {
    if (__sync_lock_test_and_set(&memdump_busy, 1)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, memdump_thread, NULL) == 0) pthread_detach(t);
}
