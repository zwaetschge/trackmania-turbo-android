/*
 * k32_file.c - Windows paths onto the host file system, file handles,
 * directory enumeration, file mappings.
 *
 * Guest view:  C:\TrackmaniaTurbo\...          -> g_game_dir (read-mostly)
 *              C:\Users\Player\...              -> g_user_dir/Users/Player/...
 *              C:\ProgramData\...               -> g_user_dir/ProgramData/...
 *              C:\Temp\...                      -> g_temp_dir/...
 *              other drive paths                -> g_user_dir/drive_<x>/...
 * Lookups are case-insensitive (Windows semantics) on case-sensitive hosts.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>

char g_game_dir[1024], g_user_dir[1024], g_temp_dir[1024];
static char g_cwd_win[512] = "C:\\TrackmaniaTurbo";

void tm_path_set_roots(const char *game, const char *user, const char *temp) {
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game);
    snprintf(g_user_dir, sizeof g_user_dir, "%s", user);
    snprintf(g_temp_dir, sizeof g_temp_dir, "%s", temp);
    mkdir(g_user_dir, 0755);
    mkdir(g_temp_dir, 0755);
}

static int prefix_ci(const char *s, const char *p) { return !strncasecmp(s, p, strlen(p)) && (s[strlen(p)] == 0 || s[strlen(p)] == '/'); }

/* Resolve each component case-insensitively below `root` (already exists). */
static void resolve_ci(char *path, size_t root_len) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    char out[2048];
    size_t olen = root_len;
    memcpy(out, path, root_len); out[olen] = 0;
    const char *p = path + root_len;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t clen = e ? (size_t)(e - p) : strlen(p);
        char comp[512];
        if (clen >= sizeof comp) clen = sizeof comp - 1;
        memcpy(comp, p, clen); comp[clen] = 0;
        char cand[2048];
        snprintf(cand, sizeof cand, "%s/%s", out, comp);
        if (stat(cand, &st) != 0) {
            DIR *d = opendir(olen ? out : "/");
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) if (!strcasecmp(de->d_name, comp)) { snprintf(comp, sizeof comp, "%s", de->d_name); break; }
                closedir(d);
            }
        }
        olen = (size_t)snprintf(out + olen, sizeof out - olen, "/%s", comp) + olen;
        p += clen;
    }
    snprintf(path, 2048, "%s", out);
}

/* Normalise a Windows path: absolute, backslashes -> '/', drop . and .. */
static void win_normalize(const char *in, char *out, size_t cap) {
    char tmp[2048];
    if (!in) in = "";
    if (!strncmp(in, "\\\\?\\", 4)) in += 4;
    if (in[0] && in[1] == ':') snprintf(tmp, sizeof tmp, "%s", in);
    else if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof tmp, "C:%s", in);
    else snprintf(tmp, sizeof tmp, "%s\\%s", g_cwd_win, in);
    for (char *c = tmp; *c; c++) if (*c == '/') *c = '\\';
    /* split into components */
    char *comps[256]; int n = 0;
    char *save = NULL;
    char drive = (char)toupper((unsigned char)tmp[0]);
    for (char *t = strtok_r(tmp + 2, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 256) comps[n++] = t;
    }
    size_t k = (size_t)snprintf(out, cap, "%c:", drive);
    for (int i = 0; i < n && k < cap; i++) k += (size_t)snprintf(out + k, cap - k, "\\%s", comps[i]);
    if (n == 0 && k < cap) snprintf(out + k, cap - k, "\\");
}

int tm_path_to_host(const char *winpath, char *out, size_t cap) {
    char norm[2048];
    win_normalize(winpath, norm, sizeof norm);
    char rest[2048];
    snprintf(rest, sizeof rest, "%s", norm + 2);  /* "\..." */
    for (char *c = rest; *c; c++) if (*c == '\\') *c = '/';
    char host[2048];
    size_t root_len;
    const char *gamew = "/TrackmaniaTurbo";
    if (norm[0] == 'C' && prefix_ci(rest, gamew)) {
        snprintf(host, sizeof host, "%s%s", g_game_dir, rest + strlen(gamew));
        root_len = strlen(g_game_dir);
    } else if (norm[0] == 'C' && prefix_ci(rest, "/Temp")) {
        snprintf(host, sizeof host, "%s%s", g_temp_dir, rest + 5);
        root_len = strlen(g_temp_dir);
    } else if (norm[0] == 'C') {
        snprintf(host, sizeof host, "%s%s", g_user_dir, rest);
        root_len = strlen(g_user_dir);
    } else {
        snprintf(host, sizeof host, "%s/drive_%c%s", g_user_dir, tolower((unsigned char)norm[0]), rest);
        root_len = strlen(g_user_dir);
    }
    size_t L = strlen(host);
    while (L > root_len + 1 && host[L - 1] == '/') host[--L] = 0;
    resolve_ci(host, root_len);
    snprintf(out, cap, "%s", host);
    return 0;
}

static void mkdirs_parent(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* errno -> Win32 error */
static uint32_t win_err(int e) {
    switch (e) {
    case ENOENT: return 2;      /* FILE_NOT_FOUND (callers adjust to PATH_NOT_FOUND) */
    case EACCES: case EPERM: case EROFS: return 5;
    case EEXIST: return 80;     /* FILE_EXISTS */
    case ENOTDIR: return 3;
    case ENOSPC: return 112;
    case ENOTEMPTY: return 145;
    case EISDIR: return 5;
    default: return 31;
    }
}

/* ------------------------------------------------------------ file objects */
typedef struct { tm_object o; int fd; char path[1024]; int is_dir; } tm_file;
static void file_destroy(tm_object *o) { tm_file *f = (tm_file *)o; if (f->fd >= 0) close(f->fd); free(f); }

/* TM_FS_LOG=1 (diagnostics): log every directory search and every open of a map, ghost or
 * file list, to find out where the game looks for things. */
int tm_fs_log = -1;
static int fs_log(void) {
    if (tm_fs_log < 0) { const char *e = getenv("TM_FS_LOG"); tm_fs_log = e && *e && *e != '0'; }
    return tm_fs_log;
}
static uint32_t open_file(const char *winpath, uint32_t access, uint32_t disp, uint32_t attrs) {
    char host[2048];
    tm_path_to_host(winpath, host, sizeof host);
    if (fs_log() && (strcasestr(winpath, ".Map.Gbx") || strcasestr(winpath, "FileList") || strcasestr(winpath, "MapsGhosts") || strcasestr(winpath, "\\Maps\\")))
        TM_INFO("fs: CreateFile(%s, access %X, disp %u) -> %s", winpath, access, disp, host);
    int flags = 0;
    int wr = (access & 0x40000000u) || (access & 0x10000000u) || (access & 0x2) || (access & 0x4);
    int rd = (access & 0x80000000u) || (access & 0x10000000u) || (access & 0x1) || !wr;
    flags = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;           /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;          /* CREATE_ALWAYS */
    case 3: break;                                      /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;                    /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;                    /* TRUNCATE_EXISTING */
    }
    struct stat st;
    int existed = stat(host, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (attrs & 0x02000000u) { /* FILE_FLAG_BACKUP_SEMANTICS: directory handle */
            tm_file *f = calloc(1, sizeof *f);
            f->o.kind = TMO_FILE; f->o.destroy = file_destroy; f->fd = -1; f->is_dir = 1;
            snprintf(f->path, sizeof f->path, "%s", host);
            return tm_handle_new(&f->o);
        }
        tm_set_last_error(5); return 0xFFFFFFFFu;
    }
    if (flags & O_CREAT) mkdirs_parent(host);
    int fd = open(host, flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        uint32_t e = win_err(errno);
        if (errno == ENOENT) { char par[2048]; snprintf(par, sizeof par, "%s", host); char *s = strrchr(par, '/'); if (s) { *s = 0; if (stat(par, &st) != 0) e = 3; } }
        TM_INFO("CreateFile(%s -> %s, acc %X, disp %u) failed: %s", winpath, host, access, disp, strerror(errno));
        tm_set_last_error(e);
        return 0xFFFFFFFFu;
    }
    tm_file *f = calloc(1, sizeof *f);
    f->o.kind = TMO_FILE; f->o.destroy = file_destroy; f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = tm_handle_new(&f->o);
    TM_INFO("CreateFile(%s, acc %X, disp %u) -> %08X", winpath, access, disp, h);
    tm_set_last_error((existed && (disp == 2 || disp == 4)) ? 183 : 0);
    return h;
}

static void shim_CreateFileW(void) {
    char p[1024];
    RET(open_file(tm_w2u(ARG(0), p, sizeof p), ARG(1), ARG(4), ARG(5)), 7);
}
static void shim_CreateFileA(void) { RET(open_file(ARGS(0), ARG(1), ARG(4), ARG(5)), 7); }

static tm_file *getf(uint32_t h) {
    if (h == 0x10 || h == 0x14 || h == 0x18) return NULL;
    return (tm_file *)tm_handle_get(h, TMO_FILE);
}

static void shim_ReadFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), got = ARG(3);
    tm_file *f = getf(h);
    if (!f || f->fd < 0) { if (got) wr32(got, 0); tm_set_last_error(6); RET(0, 5); }
    uint64_t t0 = tm_diag_on() ? tm_now_ns() : 0;
    ssize_t r = read(f->fd, G2H(buf), n);
    if (t0) tm_diag_add(1, tm_now_ns() - t0);
    if (r < 0) { if (got) wr32(got, 0); tm_set_last_error(win_err(errno)); RET(0, 5); }
    if (got) wr32(got, (uint32_t)r);
    RET(1, 5);
}
static void shim_WriteFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), put = ARG(3);
    if (h == 0x14 || h == 0x18) { fwrite(G2H(buf), 1, n, stderr); if (put) wr32(put, n); RET(1, 5); }
    tm_file *f = getf(h);
    if (!f || f->fd < 0) { tm_set_last_error(6); RET(0, 5); }
    ssize_t r = write(f->fd, G2H(buf), n);
    if (r < 0) { tm_set_last_error(win_err(errno)); RET(0, 5); }
    if (put) wr32(put, (uint32_t)r);
    RET(1, 5);
}
/* Overlapped variants: performed synchronously, then the completion routine
 * would have to be queued as an APC. Not needed so far: stop if used. */
static void shim_ReadFileEx(void) { tm_fatal("ReadFileEx (overlapped I/O with completion routine) not implemented, from %08X", g_cur_func); }
static void shim_WriteFileEx(void) { tm_fatal("WriteFileEx not implemented, from %08X", g_cur_func); }
static void shim_CancelIo(void) { RET(1, 1); }

static void shim_SetFilePointer(void) {
    uint32_t h = ARG(0), hi_p = ARG(2), method = ARG(3);
    int64_t off = (int32_t)ARG(1);
    if (hi_p) off = (int64_t)(((uint64_t)rd32(hi_p) << 32) | ARG(1));
    tm_file *f = getf(h);
    if (!f || f->fd < 0) { tm_set_last_error(6); RET(0xFFFFFFFFu, 4); }
    off_t r = lseek(f->fd, (off_t)off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { tm_set_last_error(131); RET(0xFFFFFFFFu, 4); }
    if (hi_p) wr32(hi_p, (uint32_t)((uint64_t)r >> 32));
    tm_set_last_error(0);
    RET((uint32_t)r, 4);
}
static void shim_SetFilePointerEx(void) {
    uint32_t h = ARG(0), newp = ARG(3), method = ARG(4);
    int64_t off = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    tm_file *f = getf(h);
    if (!f || f->fd < 0) { tm_set_last_error(6); RET(0, 5); }
    off_t r = lseek(f->fd, (off_t)off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { tm_set_last_error(131); RET(0, 5); }
    if (newp) { uint64_t v = (uint64_t)r; memcpy(G2H(newp), &v, 8); }
    RET(1, 5);
}
static void shim_GetFileSizeEx(void) {
    tm_file *f = getf(ARG(0));
    struct stat st;
    if (!f || f->fd < 0 || fstat(f->fd, &st)) { tm_set_last_error(6); RET(0, 2); }
    uint64_t v = (uint64_t)st.st_size; memcpy(G2H(ARG(1)), &v, 8);
    RET(1, 2);
}
static void shim_GetFileType(void) {
    uint32_t h = ARG(0);
    if (h == 0x10 || h == 0x14 || h == 0x18) RET(2, 1);   /* FILE_TYPE_CHAR */
    RET(getf(h) ? 1 : 0, 1);
}
static void shim_FlushFileBuffers(void) { RET(1, 1); }
static void shim_SetEndOfFile(void) {
    tm_file *f = getf(ARG(0));
    if (!f || f->fd < 0) RET(0, 1);
    off_t cur = lseek(f->fd, 0, SEEK_CUR);
    RET(ftruncate(f->fd, cur) == 0, 1);
}

static uint64_t ft_from_ts(struct timespec ts) { return (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull; }
static uint32_t attrs_of(struct stat *st) {
    uint32_t a = S_ISDIR(st->st_mode) ? 0x10 : 0x80;   /* DIRECTORY : NORMAL */
    if (!S_ISDIR(st->st_mode) && !(st->st_mode & 0200)) a = 0x1;
    return a;
}
static void shim_GetFileTime(void) {
    tm_file *f = getf(ARG(0)); struct stat st;
    if (!f || (f->fd >= 0 ? fstat(f->fd, &st) : stat(f->path, &st))) RET(0, 4);
    uint64_t c = ft_from_ts(st.st_ctim), a = ft_from_ts(st.st_atim), m = ft_from_ts(st.st_mtim);
    if (ARG(1)) memcpy(G2H(ARG(1)), &c, 8);
    if (ARG(2)) memcpy(G2H(ARG(2)), &a, 8);
    if (ARG(3)) memcpy(G2H(ARG(3)), &m, 8);
    RET(1, 4);
}
static void shim_SetFileTime(void) { RET(1, 4); }
static void shim_GetFileInformationByHandle(void) {
    tm_file *f = getf(ARG(0)); struct stat st; uint32_t p = ARG(1);
    if (!f || (f->fd >= 0 ? fstat(f->fd, &st) : stat(f->path, &st))) RET(0, 2);
    memset(G2H(p), 0, 52);
    wr32(p, attrs_of(&st));
    uint64_t c = ft_from_ts(st.st_ctim), a = ft_from_ts(st.st_atim), m = ft_from_ts(st.st_mtim);
    memcpy(G2H(p + 4), &c, 8); memcpy(G2H(p + 12), &a, 8); memcpy(G2H(p + 20), &m, 8);
    wr32(p + 28, 0x12345678);
    wr32(p + 32, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(p + 36, (uint32_t)st.st_size);
    wr32(p + 40, (uint32_t)st.st_nlink);
    wr32(p + 44, (uint32_t)((uint64_t)st.st_ino >> 32)); wr32(p + 48, (uint32_t)st.st_ino);
    RET(1, 2);
}

static uint32_t get_attrs(const char *winpath) {
    char host[2048]; struct stat st;
    tm_path_to_host(winpath, host, sizeof host);
    if (stat(host, &st)) { tm_set_last_error(2); return 0xFFFFFFFFu; }
    return attrs_of(&st);
}
static void shim_GetFileAttributesA(void) { RET(get_attrs(ARGS(0)), 1); }
static void shim_GetFileAttributesW(void) { char p[1024]; RET(get_attrs(tm_w2u(ARG(0), p, sizeof p)), 1); }
static int fill_attr_data(const char *winpath, uint32_t out) {
    char host[2048]; struct stat st;
    tm_path_to_host(winpath, host, sizeof host);
    if (stat(host, &st)) { tm_set_last_error(2); return 0; }
    wr32(out, attrs_of(&st));
    uint64_t c = ft_from_ts(st.st_ctim), a = ft_from_ts(st.st_atim), m = ft_from_ts(st.st_mtim);
    memcpy(G2H(out + 4), &c, 8); memcpy(G2H(out + 12), &a, 8); memcpy(G2H(out + 20), &m, 8);
    wr32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(out + 32, (uint32_t)st.st_size);
    return 1;
}
static void shim_GetFileAttributesExA(void) { RET(fill_attr_data(ARGS(0), ARG(2)), 3); }
static void shim_GetFileAttributesExW(void) { char p[1024]; RET(fill_attr_data(tm_w2u(ARG(0), p, sizeof p), ARG(2)), 3); }
static void shim_SetFileAttributesA(void) { RET(1, 2); }
static void shim_SetFileAttributesW(void) { RET(1, 2); }

static void shim_CreateDirectoryW(void) {
    char p[1024], host[2048]; tm_w2u(ARG(0), p, sizeof p);
    tm_path_to_host(p, host, sizeof host);
    mkdirs_parent(host);
    if (mkdir(host, 0755) != 0) { tm_set_last_error(errno == EEXIST ? 183 : win_err(errno)); RET(0, 2); }
    RET(1, 2);
}
static void shim_RemoveDirectoryW(void) { char p[1024], host[2048]; tm_path_to_host(tm_w2u(ARG(0), p, sizeof p), host, sizeof host); if (rmdir(host)) { tm_set_last_error(win_err(errno)); RET(0, 1); } RET(1, 1); }
static void shim_DeleteFileW(void) { char p[1024], host[2048]; tm_path_to_host(tm_w2u(ARG(0), p, sizeof p), host, sizeof host); if (unlink(host)) { tm_set_last_error(win_err(errno)); RET(0, 1); } RET(1, 1); }
static int move_file(uint32_t a, uint32_t b, int replace) {
    char p[1024], q[1024], ha[2048], hb[2048]; struct stat st;
    tm_path_to_host(tm_w2u(a, p, sizeof p), ha, sizeof ha);
    tm_path_to_host(tm_w2u(b, q, sizeof q), hb, sizeof hb);
    if (!replace && stat(hb, &st) == 0) { tm_set_last_error(183); return 0; }
    mkdirs_parent(hb);
    if (rename(ha, hb)) { tm_set_last_error(win_err(errno)); return 0; }
    return 1;
}
static void shim_MoveFileW(void) { RET(move_file(ARG(0), ARG(1), 0), 2); }
static void shim_MoveFileExW(void) { RET(move_file(ARG(0), ARG(1), ARG(2) & 1), 3); }
static void shim_CopyFileW(void) {
    char p[1024], q[1024], ha[2048], hb[2048]; struct stat st;
    tm_path_to_host(tm_w2u(ARG(0), p, sizeof p), ha, sizeof ha);
    tm_path_to_host(tm_w2u(ARG(1), q, sizeof q), hb, sizeof hb);
    if (ARG(2) && stat(hb, &st) == 0) { tm_set_last_error(80); RET(0, 3); }
    FILE *in = fopen(ha, "rb"); if (!in) { tm_set_last_error(2); RET(0, 3); }
    mkdirs_parent(hb);
    FILE *out = fopen(hb, "wb"); if (!out) { fclose(in); tm_set_last_error(5); RET(0, 3); }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    RET(1, 3);
}

/* GetFullPathName: normalise against the current directory. */
static void shim_GetFullPathNameA(void) {
    char norm[2048]; win_normalize(ARGS(0), norm, sizeof norm);
    uint32_t n = ARG(1), buf = ARG(2), fp = ARG(3);
    uint32_t len = (uint32_t)strlen(norm);
    if (n <= len) RET(len + 1, 4);
    memcpy(G2H(buf), norm, len + 1);
    if (fp) { char *s = strrchr(norm, '\\'); wr32(fp, s ? buf + (uint32_t)(s + 1 - norm) : 0); }
    RET(len, 4);
}
static void shim_GetFullPathNameW(void) {
    char in[1024], norm[2048]; win_normalize(tm_w2u(ARG(0), in, sizeof in), norm, sizeof norm);
    uint32_t n = ARG(1), buf = ARG(2), fp = ARG(3);
    uint16_t w[2048]; int len = tm_utf8_to_utf16(norm, w, 2048);
    if (n <= (uint32_t)len) RET(len + 1, 4);
    memcpy(G2H(buf), w, (size_t)(len + 1) * 2);
    if (fp) { int k = len; while (k > 0 && w[k - 1] != '\\') k--; wr32(fp, buf + 2 * (uint32_t)k); }
    RET(len, 4);
}
static void shim_GetCurrentDirectoryW(void) {
    uint32_t n = ARG(0), buf = ARG(1);
    uint32_t len = (uint32_t)strlen(g_cwd_win);
    if (n <= len) RET(len + 1, 2);
    tm_u2w(g_cwd_win, buf, (int)n);
    RET(len, 2);
}
static void shim_GetShortPathNameW(void) {
    uint32_t len = (uint32_t)tm_wcslen(ARG(0));
    if (ARG(2) > len) memcpy(G2H(ARG(1)), G2H(ARG(0)), (len + 1) * 2);
    RET(ARG(2) > len ? len : len + 1, 3);
}
static void shim_GetTempFileNameW(void) {
    static uint32_t counter = 1;
    char dir[1024]; tm_w2u(ARG(0), dir, sizeof dir);
    char name[1200]; snprintf(name, sizeof name, "%s\\tm%04X.tmp", dir, counter++);
    tm_u2w(name, ARG(3), 260);
    RET(counter - 1, 4);
}
static void shim_SearchPathW(void) { tm_set_last_error(2); RET(0, 6); }
static void shim_GetDriveTypeW(void) { RET(3, 1); }   /* DRIVE_FIXED */
static void shim_GetDiskFreeSpaceExW(void) {
    struct statvfs v; uint64_t fr = 8ull << 30, tot = 64ull << 30;
    if (statvfs(g_user_dir, &v) == 0) { fr = (uint64_t)v.f_bavail * v.f_frsize; tot = (uint64_t)v.f_blocks * v.f_frsize; }
    if (ARG(1)) memcpy(G2H(ARG(1)), &fr, 8);
    if (ARG(2)) memcpy(G2H(ARG(2)), &tot, 8);
    if (ARG(3)) memcpy(G2H(ARG(3)), &fr, 8);
    RET(1, 4);
}
static void shim_GetVolumeInformationW(void) {
    if (ARG(1) && ARG(2)) tm_u2w("ANDROID", ARG(1), (int)ARG(2));
    if (ARG(3)) wr32(ARG(3), 0x1A2B3C4D);
    if (ARG(4)) wr32(ARG(4), 255);
    if (ARG(5)) wr32(ARG(5), 0x3);
    if (ARG(6) && ARG(7)) tm_u2w("NTFS", ARG(6), (int)ARG(7));
    RET(1, 8);
}
static void shim_FindFirstVolumeW(void) { tm_set_last_error(18); RET(0xFFFFFFFFu, 2); }
static void shim_FindNextVolumeW(void) { tm_set_last_error(18); RET(0, 3); }
static void shim_FindVolumeClose(void) { RET(1, 1); }

/* ------------------------------------------------------------ find */
typedef struct { tm_object o; char dir[2048]; char pattern[512]; DIR *d; } tm_find;
static void find_destroy(tm_object *o) { tm_find *f = (tm_find *)o; if (f->d) closedir(f->d); free(f); }

/* WIN32_FIND_DATAW: attrs(4) 3xFILETIME(24) sizeHi sizeLo res0 res1 name[260] alt[14] = 592 bytes */
static int find_next(tm_find *f, uint32_t out) {
    struct dirent *de;
    while ((de = readdir(f->d))) {
        if (fnmatch(f->pattern, de->d_name, FNM_CASEFOLD) != 0 && strcmp(f->pattern, "*.*") != 0) continue;
        if (!strcmp(f->pattern, "*.*") && 0) continue;
        char full[2600]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, de->d_name);
        if (stat(full, &st)) continue;
        memset(G2H(out), 0, 592);
        wr32(out, attrs_of(&st));
        uint64_t c = ft_from_ts(st.st_ctim), a = ft_from_ts(st.st_atim), m = ft_from_ts(st.st_mtim);
        memcpy(G2H(out + 4), &c, 8); memcpy(G2H(out + 12), &a, 8); memcpy(G2H(out + 20), &m, 8);
        wr32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(out + 32, (uint32_t)st.st_size);
        tm_u2w(de->d_name, out + 44, 260);
        return 1;
    }
    return 0;
}
static uint32_t find_first(const char *winpat, uint32_t out) {
    if (fs_log()) TM_INFO("fs: FindFirstFile(%s)", winpat);
    char norm[2048]; win_normalize(winpat, norm, sizeof norm);
    char *bs = strrchr(norm, '\\');
    tm_find *f = calloc(1, sizeof *f);
    f->o.kind = TMO_FIND; f->o.destroy = find_destroy;
    snprintf(f->pattern, sizeof f->pattern, "%s", bs ? bs + 1 : "*");
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    if (bs) *bs = 0;
    tm_path_to_host(norm, f->dir, sizeof f->dir);
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, out)) {
        TM_TRACE("FindFirstFile(%s) -> none (dir %s)", winpat, f->dir);
        find_destroy(&f->o);
        tm_set_last_error(2);
        return 0xFFFFFFFFu;
    }
    return tm_handle_new(&f->o);
}
static void shim_FindFirstFileW(void) { char p[1024]; RET(find_first(tm_w2u(ARG(0), p, sizeof p), ARG(1)), 2); }
static void shim_FindFirstFileExW(void) { char p[1024]; RET(find_first(tm_w2u(ARG(0), p, sizeof p), ARG(2)), 6); }
static void shim_FindNextFileW(void) {
    tm_find *f = (tm_find *)tm_handle_get(ARG(0), TMO_FIND);
    if (!f) { tm_set_last_error(6); RET(0, 2); }
    if (!find_next(f, ARG(1))) { tm_set_last_error(18); RET(0, 2); }
    RET(1, 2);
}
static void shim_FindClose(void) { RET(tm_handle_close(ARG(0)), 1); }

/* ------------------------------------------------------------ file mappings */
typedef struct { tm_object o; uint32_t file; uint32_t size; char name[128]; } tm_mapping;
static void shim_CreateFileMappingW(void) {
    uint32_t file = ARG(0);
    uint64_t sz = ((uint64_t)ARG(3) << 32) | ARG(4);
    tm_file *f = file != 0xFFFFFFFFu ? getf(file) : NULL;
    if (!sz && f) { struct stat st; fstat(f->fd, &st); sz = (uint64_t)st.st_size; }
    tm_mapping *m = calloc(1, sizeof *m);
    m->o.kind = TMO_MAPPING; m->file = file; m->size = (uint32_t)sz;
    if (f) tm_handle_dup(file);
    RET(tm_handle_new(&m->o), 6);
}
static void shim_OpenFileMappingA(void) { tm_set_last_error(2); RET(0, 3); }
static void shim_MapViewOfFile(void) {
    tm_mapping *m = (tm_mapping *)tm_handle_get(ARG(0), TMO_MAPPING);
    if (!m) { tm_set_last_error(6); RET(0, 5); }
    uint64_t off = ((uint64_t)ARG(2) << 32) | ARG(3);
    uint32_t len = ARG(4) ? ARG(4) : m->size - (uint32_t)off;
    uint32_t va = tm_vm_alloc(0, len, 0x3000, 4);
    if (!va) RET(0, 5);
    tm_file *f = m->file != 0xFFFFFFFFu ? getf(m->file) : NULL;
    if (f) { ssize_t r = pread(f->fd, G2H(va), len, (off_t)off); (void)r; }
    TM_INFO("MapViewOfFile %u bytes at %08X (copy, writes are not flushed back)", len, va);
    RET(va, 5);
}
static void shim_UnmapViewOfFile(void) { RET(tm_vm_free(ARG(0), 0, 0x8000), 1); }

/* ------------------------------------------------------------ ini */
static void shim_GetPrivateProfileStringA(void) {
    const char *def = ARGS(2);
    uint32_t buf = ARG(3), n = ARG(4);
    const char *sec = ARGS(0), *key = ARGS(1), *file = ARGS(5);
    char host[2048]; tm_path_to_host(file, host, sizeof host);
    FILE *fp = fopen(host, "r");
    char line[1024], cur[256] = "";
    const char *val = NULL; static char found[1024];
    if (fp && sec && key) {
        while (fgets(line, sizeof line, fp)) {
            char *s = line; while (isspace((unsigned char)*s)) s++;
            char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
            if (*s == ';' || !*s) continue;
            if (*s == '[') { char *r = strchr(s, ']'); if (r) *r = 0; snprintf(cur, sizeof cur, "%s", s + 1); continue; }
            char *eq = strchr(s, '=');
            if (!eq || strcasecmp(cur, sec)) continue;
            *eq = 0; char *k = s, *ke = eq; while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
            if (!strcasecmp(k, key)) { char *v = eq + 1; while (isspace((unsigned char)*v)) v++; snprintf(found, sizeof found, "%s", v); val = found; break; }
        }
    }
    if (fp) fclose(fp);
    if (!val) val = def ? def : "";
    uint32_t len = (uint32_t)strlen(val);
    if (!n) RET(0, 6);
    if (len >= n) len = n - 1;
    memcpy(G2H(buf), val, len); wr8(buf + len, 0);
    TM_INFO("GetPrivateProfileString([%s] %s, %s) = '%s'", sec ? sec : "", key ? key : "", file ? file : "", val);
    RET(len, 6);
}

static const tm_shim_def k32_file_shims[] = {
#define K(n) TM_SHIM("kernel32.dll", n)
    K(CreateFileW), K(CreateFileA), K(ReadFile), K(WriteFile), K(ReadFileEx), K(WriteFileEx), K(CancelIo),
    K(SetFilePointer), K(SetFilePointerEx), K(GetFileSizeEx), K(GetFileType), K(FlushFileBuffers), K(SetEndOfFile),
    K(GetFileTime), K(SetFileTime), K(GetFileInformationByHandle), K(GetFileAttributesA), K(GetFileAttributesW),
    K(GetFileAttributesExA), K(GetFileAttributesExW), K(SetFileAttributesA), K(SetFileAttributesW),
    K(CreateDirectoryW), K(RemoveDirectoryW), K(DeleteFileW), K(MoveFileW), K(MoveFileExW), K(CopyFileW),
    K(GetFullPathNameA), K(GetFullPathNameW), K(GetCurrentDirectoryW), K(GetShortPathNameW), K(GetTempFileNameW),
    K(SearchPathW), K(GetDriveTypeW), K(GetDiskFreeSpaceExW), K(GetVolumeInformationW), K(FindFirstVolumeW),
    K(FindNextVolumeW), K(FindVolumeClose), K(FindFirstFileW), K(FindFirstFileExW), K(FindNextFileW), K(FindClose),
    K(CreateFileMappingW), K(OpenFileMappingA), K(MapViewOfFile), K(UnmapViewOfFile), K(GetPrivateProfileStringA),
#undef K
};
void tm_k32_file_register(void) { tm_register_shims(k32_file_shims, sizeof k32_file_shims / sizeof k32_file_shims[0]); }
