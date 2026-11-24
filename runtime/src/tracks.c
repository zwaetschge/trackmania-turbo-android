/*
 * tracks.c - community tracks (Track Lab maps shared by players) for the offline game.
 *
 * The game's online track browser ("Trackbuilder Workshop") needs Ubisoft's services. The
 * tracks themselves are public files (tools/ugc_fetch.py downloads them), and the game
 * lists every map of the player's profile under TRACKBUILDER -> MANAGE YOUR TRACKS
 * ("My Tracks"), from where they can be played. The profile keeps its maps and ghosts in
 * Documents\TrackmaniaTurbo\<profile>\MapsGhosts as numbered files plus an index,
 * FileList.Gbx; only what the index lists is shown (files dropped into the folder, or into
 * Documents\TrackmaniaTurbo\Maps, are not).
 *
 * Android does not let adb or other apps write into the folders the game created, so tracks
 * are dropped into <user dir>/ImportTracks/ (subfolders are fine). At start-up every map
 * there whose uid the profile does not have yet is copied into MapsGhosts as <n>.Map.Gbx
 * and added to FileList.Gbx; the entries that are there (the player's own tracks, best
 * ghosts) are kept as they are. The first time, the original index is kept as
 * FileList.Gbx.before-import. TM_TRACKS_IMPORT=0 skips the step. TM_TRACKS_MAX=<n> keeps at
 * most n imported tracks in the list (the first n in name order; the others are not
 * copied, or are removed again when the limit goes down).
 *
 * FileList.Gbx (class 0x031B7000, read by 0x009ACD00): GBX version 6 "BUCR", one chunk
 * 0x031B7000 { u32 version (1), u32 count, entries }, then 0xFACADE01; the body is LZO1X
 * compressed. Entry: name (u32 len, len chars, NUL; < 32 bytes), FILETIME u64, size u64,
 * kind u32; kind 0 = map: mapUid (GBX Id), map name (as name); kind 1 = ghost: ghost name
 * (as name), mapUid (Id), time u32 (ms). The first Id of the body is preceded by the Id
 * version, 3.
 */
#include "tmrt.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char g_user_dir[1024];

#define PROFILE_DIR "C:\\Users\\Player\\Documents\\TrackmaniaTurbo\\00000000-0000-0000-0000-000000000000"

/* ------------------------------------------------------------------ small helpers */
static int mkdirs(const char *path) {
    char p[2048]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0775); *s = '/'; }
    return mkdir(p, 0775) == 0 || errno == EEXIST ? 0 : -1;
}

static uint8_t *read_file(const char *path, size_t *len, size_t max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || (size_t)st.st_size > max) { close(fd); return NULL; }
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!b || got != (size_t)st.st_size) { free(b); return NULL; }
    *len = got;
    return b;
}

static int write_file_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[2100]; snprintf(tmp, sizeof tmp, "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) return -1;
    size_t put = 0;
    while (put < len) { ssize_t n = write(fd, data + put, len - put); if (n <= 0) break; put += (size_t)n; }
    int ok = put == len;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    size_t n; uint8_t *b = read_file(src, &n, 64u << 20);
    if (!b) return -1;
    int r = write_file_atomic(dst, b, n);
    free(b);
    return r;
}

/* ------------------------------------------------------------------ LZO1X */
/* Decompressor for the LZO1X format (as produced by lzo1x_1_compress), own implementation.
 * Returns the number of bytes written, or -1 on a malformed stream. */
long tm_lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    const uint8_t *ip = in, *ip_end = in + in_len;
    uint8_t *op = out, *op_end = out + out_cap;
    const uint8_t *m_pos;
    size_t t;
#define NEED_IP(n) do { if ((size_t)(ip_end - ip) < (size_t)(n)) return -1; } while (0)
#define NEED_OP(n) do { if ((size_t)(op_end - op) < (size_t)(n)) return -1; } while (0)
#define LIT(n) do { NEED_IP(n); NEED_OP(n); memcpy(op, ip, n); op += n; ip += n; } while (0)
#define EXT(base) do { for (;;) { NEED_IP(1); if (*ip) break; t += 255; ip++; } t += (base) + *ip++; } while (0)
    NEED_IP(1);
    if (*ip > 17) {
        t = (size_t)*ip++ - 17;
        if (t < 4) goto match_next;
        LIT(t);
        goto first_literal_run;
    }
    for (;;) {
        NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        if (t == 0) EXT(15);
        LIT(t + 3);
first_literal_run:
        NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        NEED_IP(1);
        if ((size_t)(op - out) < 1 + 0x0800 + (t >> 2) + ((size_t)*ip << 2)) return -1;
        m_pos = op - (1 + 0x0800) - (t >> 2) - ((size_t)*ip++ << 2);
        NEED_OP(3);
        *op++ = *m_pos++; *op++ = *m_pos++; *op++ = *m_pos;
        goto match_done;
        for (;;) {
            size_t dist;
match:
            if (t >= 64) {
                NEED_IP(1);
                dist = 1 + ((t >> 2) & 7) + ((size_t)*ip++ << 3);
                t = (t >> 5) - 1;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) EXT(31);
                NEED_IP(2);
                dist = 1 + ((size_t)(ip[0] >> 2) + ((size_t)ip[1] << 6));
                ip += 2;
            } else if (t >= 16) {
                dist = (t & 8) << 11;
                t &= 7;
                if (t == 0) EXT(7);
                NEED_IP(2);
                dist += (size_t)(ip[0] >> 2) + ((size_t)ip[1] << 6);
                ip += 2;
                if (dist == 0) return (long)(op - out);            /* end of stream */
                dist += 0x4000;
            } else {
                NEED_IP(1);
                dist = 1 + (t >> 2) + ((size_t)*ip++ << 2);
                if ((size_t)(op - out) < dist) return -1;
                m_pos = op - dist;
                NEED_OP(2);
                *op++ = *m_pos++; *op++ = *m_pos;
                goto match_done;
            }
            if ((size_t)(op - out) < dist) return -1;
            m_pos = op - dist;
            NEED_OP(t + 2);
            for (size_t i = 0; i < t + 2; i++) *op++ = *m_pos++;
match_done:
            t = ip[-2] & 3;
            if (t == 0) break;
match_next:
            LIT(t);
            NEED_IP(1);
            t = *ip++;
        }
    }
#undef NEED_IP
#undef NEED_OP
#undef LIT
#undef EXT
}

/* "Compression" into a valid LZO1X stream that holds the data as one literal run. */
size_t tm_lzo1x_store(const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t *op = out;
    if (n >= 4 && n <= 238) *op++ = (uint8_t)(n + 17);
    else if (n > 238) {
        *op++ = 0;
        size_t rem = n - 18;
        while (rem > 255) { *op++ = 0; rem -= 255; }
        *op++ = (uint8_t)rem;
    } else if (n > 0) *op++ = (uint8_t)(n + 17);               /* 1..3: copied via the match_next path */
    memcpy(op, in, n); op += n;
    *op++ = 0x11; *op++ = 0; *op++ = 0;                          /* end of stream */
    return (size_t)(op - out);
}

/* ------------------------------------------------------------------ the index */
typedef struct {
    char name[32];
    uint64_t filetime, size;
    uint32_t kind;
    char uid[64];
    char text[32];        /* kind 0: map name; kind 1: ghost name */
    uint32_t time;        /* kind 1 */
} entry;

typedef struct { const uint8_t *p, *end; int err; int ids_seen; char ids[64][64]; int nids; } rd;

static uint32_t rd_u32(rd *r) { if (r->end - r->p < 4) { r->err = 1; return 0; } uint32_t v; memcpy(&v, r->p, 4); r->p += 4; return v; }
static uint64_t rd_u64(rd *r) { uint64_t lo = rd_u32(r); return lo | ((uint64_t)rd_u32(r) << 32); }
static void rd_fstr(rd *r, char out[32]) {                       /* u32 len, chars, NUL */
    uint32_t n = rd_u32(r);
    if (n >= 32 || (size_t)(r->end - r->p) < (size_t)n + 1) { r->err = 1; out[0] = 0; return; }
    memcpy(out, r->p, n); out[n] = 0; r->p += n + 1;
}
static void rd_id(rd *r, char *out, size_t cap) {               /* GBX Id ("lookback string") */
    out[0] = 0;
    if (!r->ids_seen) { r->ids_seen = 1; if (rd_u32(r) != 3) { r->err = 1; return; } }
    uint32_t v = rd_u32(r);
    if (r->err || v == 0xFFFFFFFFu) return;
    if ((v & 0xC0000000u) && !(v & 0x3FFFFFFFu)) {
        uint32_t n = rd_u32(r);
        if (n >= cap || (size_t)(r->end - r->p) < n) { r->err = 1; return; }
        memcpy(out, r->p, n); out[n] = 0; r->p += n;
        if (r->nids < 64) snprintf(r->ids[r->nids++], 64, "%s", out);
    } else if (v & 0xC0000000u) {
        uint32_t i = (v & 0x3FFFFFFFu) - 1;
        if (i < (uint32_t)r->nids) snprintf(out, cap, "%s", r->ids[i]); else r->err = 1;
    } else snprintf(out, cap, "#%u", v);                         /* a numbered id */
}

static entry *g_e; static int g_n, g_cap;

static entry *new_entry(void) {
    if (g_n == g_cap) { g_cap = g_cap ? g_cap * 2 : 256; g_e = realloc(g_e, sizeof *g_e * (size_t)g_cap); }
    entry *e = &g_e[g_n]; memset(e, 0, sizeof *e);
    return e;
}

static int parse_filelist(const uint8_t *d, size_t len) {
    if (len < 33 || memcmp(d, "GBX", 3) || d[3] != 6 || memcmp(d + 5, "BUCR", 4)) return -1;
    uint32_t cls, ud, ext, usize, csize;
    memcpy(&cls, d + 9, 4); memcpy(&ud, d + 13, 4);
    if (cls != 0x031B7000u || ud) return -1;
    memcpy(&ext, d + 21, 4); memcpy(&usize, d + 25, 4); memcpy(&csize, d + 29, 4);
    if (ext || usize > (16u << 20) || 33 + (size_t)csize > len) return -1;
    uint8_t *body = malloc((size_t)usize + 16);
    long got = body ? tm_lzo1x_decompress(d + 33, csize, body, usize) : -1;
    if (got != (long)usize) { free(body); return -1; }
    rd r = { body, body + usize, 0, 0, {{0}}, 0 };
    int ok = rd_u32(&r) == 0x031B7000u && rd_u32(&r) == 1;
    uint32_t n = rd_u32(&r);
    for (uint32_t i = 0; ok && i < n && !r.err; i++) {
        entry *e = new_entry();
        rd_fstr(&r, e->name); e->filetime = rd_u64(&r); e->size = rd_u64(&r); e->kind = rd_u32(&r);
        if (e->kind == 0) { rd_id(&r, e->uid, sizeof e->uid); rd_fstr(&r, e->text); }
        else if (e->kind == 1) { rd_fstr(&r, e->text); rd_id(&r, e->uid, sizeof e->uid); e->time = rd_u32(&r); }
        else ok = 0;
        if (!r.err && ok) g_n++;
    }
    ok = ok && !r.err && rd_u32(&r) == 0xFACADE01u;
    free(body);
    return ok ? 0 : -1;
}

typedef struct { uint8_t *b; size_t n, cap; int ids_written; } wr;
static void w_bytes(wr *w, const void *p, size_t n) {
    if (w->n + n > w->cap) { w->cap = (w->n + n) * 2 + 4096; w->b = realloc(w->b, w->cap); }
    memcpy(w->b + w->n, p, n); w->n += n;
}
static void w_u32(wr *w, uint32_t v) { w_bytes(w, &v, 4); }
static void w_u64(wr *w, uint64_t v) { w_bytes(w, &v, 8); }
static void w_fstr(wr *w, const char *s) { uint32_t n = (uint32_t)strlen(s); if (n > 31) n = 31; w_u32(w, n); w_bytes(w, s, n); w_bytes(w, "", 1); }
static void w_id(wr *w, const char *s) {
    if (!w->ids_written) { w->ids_written = 1; w_u32(w, 3); }
    if (!s[0]) { w_u32(w, 0xFFFFFFFFu); return; }
    w_u32(w, 0x40000000u); w_u32(w, (uint32_t)strlen(s)); w_bytes(w, s, strlen(s));
}

static uint8_t *build_filelist(size_t *len) {
    wr body = {0};
    w_u32(&body, 0x031B7000u); w_u32(&body, 1); w_u32(&body, (uint32_t)g_n);
    for (int i = 0; i < g_n; i++) {
        entry *e = &g_e[i];
        w_fstr(&body, e->name); w_u64(&body, e->filetime); w_u64(&body, e->size); w_u32(&body, e->kind);
        if (e->kind == 0) { w_id(&body, e->uid); w_fstr(&body, e->text); }
        else { w_fstr(&body, e->text); w_id(&body, e->uid); w_u32(&body, e->time); }
    }
    w_u32(&body, 0xFACADE01u);
    uint8_t *c = malloc(body.n + body.n / 255 + 64);
    size_t cn = tm_lzo1x_store(body.b, body.n, c);
    wr f = {0};
    w_bytes(&f, "GBX", 3); uint16_t v = 6; w_bytes(&f, &v, 2); w_bytes(&f, "BUCR", 4);
    w_u32(&f, 0x031B7000u); w_u32(&f, 0); w_u32(&f, 1); w_u32(&f, 0);
    w_u32(&f, (uint32_t)body.n); w_u32(&f, (uint32_t)cn); w_bytes(&f, c, cn);
    free(c); free(body.b);
    *len = f.n;
    return f.b;
}

/* ------------------------------------------------------------------ map headers */
/* uid and name of a map from its header chunk 0x03043003. */
static int map_info(const char *path, char *uid, size_t ucap, char *name, size_t ncap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t h[16384];
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    if (n < 21 || memcmp(h, "GBX", 3) || h[3] != 6) return -1;
    uint32_t cls, nch; memcpy(&cls, h + 9, 4);
    if (cls != 0x03043000u) return -1;
    memcpy(&nch, h + 17, 4);
    if (nch > 64 || 21 + 8 * (size_t)nch > (size_t)n) return -1;
    size_t off = 21 + 8 * (size_t)nch;
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t id, sz; memcpy(&id, h + 21 + 8 * i, 4); memcpy(&sz, h + 25 + 8 * i, 4); sz &= 0x7FFFFFFFu;
        if (id == 0x03043003u) {
            if (off + sz > (size_t)n || sz < 2) return -1;
            rd r = { h + off + 1, h + off + sz, 0, 0, {{0}}, 0 };   /* skip the chunk's version byte */
            char env[64], author[64];
            rd_id(&r, uid, ucap); rd_id(&r, env, sizeof env); rd_id(&r, author, sizeof author);
            uint32_t l = rd_u32(&r);
            if (r.err || !uid[0] || l >= ncap || (size_t)(r.end - r.p) < l) return -1;
            memcpy(name, r.p, l); name[l] = 0;
            return 0;
        }
        off += sz;
    }
    return -1;
}

/* ------------------------------------------------------------------ import */
static uint64_t filetime_now(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}

static entry *find_uid(const char *uid) {
    for (int i = 0; i < g_n; i++) if (g_e[i].kind == 0 && !strcmp(g_e[i].uid, uid)) return &g_e[i];
    return NULL;
}
static int g_relabelled;

static int g_next_num, g_added, g_known, g_bad;
static int g_limit = 1 << 30, g_ours, g_skipped;              /* TM_TRACKS_MAX, see tm_tracks_import */
static char g_mg[2048];

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Files are taken in name order, so a numbered prefix decides the order in the list. A name
 * "<rank> <label>__<anything>.Map.Gbx" (as tools/ugc_fetch.py names them) gives the entry
 * the label (e.g. "#107 Mr.DvD_TM") instead of the bare map name. */
static void import_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL; size_t nn = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (nn == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, cap * sizeof *names); }
        names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    if (nn) qsort(names, nn, sizeof *names, cmp_names);
    for (size_t k = 0; k < nn; k++) {
        const char *fn = names[k];
        char src[2048]; snprintf(src, sizeof src, "%s/%s", dir, fn);
        struct stat st;
        if (stat(src, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 3) import_dir(src, depth + 1); continue; }
        size_t l = strlen(fn);
        if (l < 9 || strcasecmp(fn + l - 8, ".Map.Gbx")) continue;
        char label[64] = "";
        const char *sp = strchr(fn, ' '), *us = strstr(fn, "__");
        if (sp && us && us > sp + 1 && (size_t)(us - sp - 1) < sizeof label) { memcpy(label, sp + 1, (size_t)(us - sp - 1)); label[us - sp - 1] = 0; }
        char uid[64], name[64];
        if (map_info(src, uid, sizeof uid, name, sizeof name)) { g_bad++; continue; }
        entry *have = find_uid(uid);
        if (have) {                                             /* known: only the label may be new */
            g_known++;
            if (label[0] && strcmp(have->text, label)) { snprintf(have->text, sizeof have->text, "%.31s", label); g_relabelled++; }
            continue;
        }
        if (g_ours >= g_limit) { g_skipped++; continue; }
        char fname[32]; snprintf(fname, sizeof fname, "%d.Map.Gbx", g_next_num);
        char dst[2100]; snprintf(dst, sizeof dst, "%s/%s", g_mg, fname);
        if (copy_file(src, dst)) { g_bad++; TM_WARN("tracks: cannot copy %s", src); continue; }
        entry *e = new_entry(); g_n++;
        snprintf(e->name, sizeof e->name, "%s", fname);
        e->filetime = filetime_now(); e->size = (uint64_t)st.st_size; e->kind = 0;
        snprintf(e->uid, sizeof e->uid, "%s", uid);
        snprintf(e->text, sizeof e->text, "%.31s", label[0] ? label : name);
        g_next_num++; g_added++; g_ours++;
    }
    for (size_t k = 0; k < nn; k++) free(names[k]);
    free(names);
}

/* Leftovers of the first experiments (2026-11-11): maps copied under their uid names into
 * MapsGhosts or Documents\TrackmaniaTurbo\Maps\My Maps; and a FileList.Gbx moved aside. */
static void remove_named_like_imports(const char *dir_win, const char *imports) {
    char dir[2048]; tm_path_to_host(dir_win, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de; int removed = 0;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 30 || strcasecmp(de->d_name + l - 8, ".Map.Gbx")) continue;   /* uid names only, never <n>.Map.Gbx */
        char imp[2100], p[2100]; struct stat st;
        snprintf(imp, sizeof imp, "%s/%s", imports, de->d_name);
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!stat(imp, &st) && !unlink(p)) removed++;
    }
    closedir(d);
    if (removed) TM_INFO("tracks: removed %d test copies from %s", removed, dir_win);
}

/* Called once at start-up, before the game looks at its files. */
void tm_tracks_import(void) {
    const char *off = getenv("TM_TRACKS_IMPORT");
    if (off && *off == '0') return;
    char src[1100]; snprintf(src, sizeof src, "%s/ImportTracks", g_user_dir);
    struct stat st;
    if (stat(src, &st) || !S_ISDIR(st.st_mode)) return;
    tm_path_to_host(PROFILE_DIR "\\MapsGhosts", g_mg, sizeof g_mg);
    if (mkdirs(g_mg)) { TM_WARN("tracks: cannot create %s", g_mg); return; }
    remove_named_like_imports(PROFILE_DIR "\\MapsGhosts", src);
    remove_named_like_imports("C:\\Users\\Player\\Documents\\TrackmaniaTurbo\\Maps\\My Maps", src);

    char fl[2100]; snprintf(fl, sizeof fl, "%s/FileList.Gbx", g_mg);
    g_n = 0;
    size_t len = 0;
    uint8_t *d = read_file(fl, &len, 64u << 20);
    if (d && parse_filelist(d, len)) {
        TM_WARN("tracks: %s is not in the expected format, no tracks imported", fl);
        free(d); return;
    }
    if (d) {
        char keep[2100]; snprintf(keep, sizeof keep, "%s/FileList.Gbx.before-import", g_mg);
        if (stat(keep, &st)) write_file_atomic(keep, d, len);
        free(d);
    }
    g_next_num = 1000;                                          /* clear of the game's own numbers */
    for (int i = 0; i < g_n; i++) { int k = atoi(g_e[i].name); if (k >= g_next_num) g_next_num = k + 1; }
    DIR *dd = opendir(g_mg);                                    /* and of files the index does not list */
    if (dd) { struct dirent *de; while ((de = readdir(dd))) { int k = atoi(de->d_name); if (k >= g_next_num) g_next_num = k + 1; } closedir(dd); }

    g_added = g_known = g_bad = g_relabelled = g_skipped = 0;
    const char *mx = getenv("TM_TRACKS_MAX");
    g_limit = mx && *mx ? atoi(mx) : 1 << 30;
    g_ours = 0;
    for (int i = 0; i < g_n; i++) if (g_e[i].kind == 0 && atoi(g_e[i].name) >= 1000) g_ours++;
    uint64_t t0 = tm_now_ns();
    import_dir(src, 0);
    /* TM_TRACKS_MAX=<n>: list at most n imported tracks (entries from 1000 up, i.e. the ones this
     * import made); the others leave the index and their files are deleted -- raising the
     * limit imports them again. */
    int dropped = 0;
    if (g_ours > g_limit) {
        int lim = g_limit, kept = 0, j = 0;
        for (int i = 0; i < g_n; i++) {
            entry *e = &g_e[i];
            int ours = e->kind == 0 && atoi(e->name) >= 1000;
            if (ours && kept++ >= lim) {
                char p[2100]; snprintf(p, sizeof p, "%s/%s", g_mg, e->name);
                unlink(p); dropped++;
                continue;
            }
            g_e[j++] = *e;
        }
        g_n = j;
    }
    if (g_added || g_relabelled || dropped) {
        size_t n; uint8_t *out = build_filelist(&n);
        if (write_file_atomic(fl, out, n)) TM_WARN("tracks: cannot write %s", fl);
        free(out);
    }
    TM_INFO("tracks: %d imported, %d already in the profile (%d renamed), %d unreadable, %d left out and %d removed for TM_TRACKS_MAX; the profile lists %d files (%.0f ms)",
            g_added, g_known, g_relabelled, g_bad, g_skipped, dropped, g_n, (tm_now_ns() - t0) / 1e6);
}
