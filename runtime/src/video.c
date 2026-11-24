/*
 * video.c - the game's VP8 video, decoded by a native libvpx.
 *
 * The game plays WebM video everywhere: the menu backgrounds (two 1920x1080 streams),
 * the logos at start-up, the track thumbnails, the billboards along the tracks. Its
 * decoder is a libvpx 1.4 linked into the exe, whose x86 SIMD routines come out of the
 * lifter as scalar C: a 1080p frame took 60-110 ms on the Odin3, so the menu videos ran
 * at 6 frames per second and two decoder threads kept more than one core busy.
 *
 * libvpx reaches a codec through an interface table (vpx_codec_iface_t). The entries
 * of the VP8 decoder are called indirectly, so overrides can replace them:
 *     decode    -> the frame goes to a native decoder (work/libvpx-android, NEON)
 *     get_frame -> hands the game a vpx_image_t in ITS layout, planes in guest memory
 *     destroy   -> frees the native decoder, then runs the game's own destroy
 * init, peek_si, get_si and the controls stay the game's: they only touch the guest
 * decoder object ("alg_priv"), which the game still allocates and frees; the hooks keep
 * the fields the others read (stream info, user pointer, the image) up to date.
 * The native decoder runs outside the machine lock like the lifted one did.
 *
 * TM_VIDEO_NATIVE=0 keeps the game's decoder; TM_VIDEO_THREADS=<n> decoder threads per
 * stream (default 2); TM_VIDEO_LOG=1 logs frame counts and decode times.
 */
#include "tmrt.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef TM_HAVE_VPX
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

/* The VP8 decoder interface of the exe (table at 0x019C2060, "WebM Project VP8 Decoder v1.4.0"). */
#define VA_VP8_DESTROY   0x0139BB20u
#define VA_VP8_DECODE    0x0139BE60u
#define VA_VP8_GET_FRAME 0x0139C280u
/* Fields of the guest decoder object that other guest code reads. */
#define CTX_SI_W      0xD8u     /* vpx_codec_stream_info_t at 0xD4: sz, w, h, is_kf */
#define CTX_SI_H      0xDCu
#define CTX_SI_KF     0xE0u
#define CTX_IMG       0x100u    /* vpx_image_t the game's get_frame returns */
#define CTX_USER_PRIV 0x1F4u
/* vpx_image_t as this libvpx lays it out (yuvconfig2image at 0x0139BCC0). */
enum { IMG_FMT = 0x00, IMG_CS = 0x04, IMG_RANGE = 0x08, IMG_W = 0x0C, IMG_H = 0x10, IMG_BIT_DEPTH = 0x14,
       IMG_D_W = 0x18, IMG_D_H = 0x1C, IMG_R_W = 0x20, IMG_R_H = 0x24, IMG_X_SHIFT = 0x28, IMG_Y_SHIFT = 0x2C,
       IMG_PLANES = 0x30, IMG_STRIDE = 0x40, IMG_BPS = 0x50, IMG_USER_PRIV = 0x54, IMG_DATA = 0x58,
       IMG_DATA_OWNER = 0x5C, IMG_SELF_ALLOCD = 0x60, IMG_FB_PRIV = 0x64 };
#define BORDER 32u              /* libvpx's frame border: the game sees the same geometry */
/* libvpx keeps a shown frame intact while it decodes the next one (it is that frame's
 * reference); a player may read it that long. So the frames go round three buffers. */
#define NBUF 3

typedef struct vdec {
    struct vdec *next;
    uint32_t gctx;                          /* the guest decoder object this one stands in for */
    vpx_codec_ctx_t codec; int inited, failed;
    uint32_t w, h;                          /* frame size the guest buffers were made for */
    uint32_t gbuf[NBUF], y_stride, uv_stride, y_off, u_off, v_off;
    int cur, have_frame;                    /* gbuf[cur] holds the newest frame */
    uint64_t frames, ns, log_frames, log_ns, log_t0;
} vdec;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vdec *g_decs;
static recomp_func_t g_orig_destroy;
static int g_native = -1, g_threads = 2, g_log;

static vdec *dec_find(uint32_t gctx, int create) {
    pthread_mutex_lock(&g_mu);
    vdec *d = g_decs;
    while (d && d->gctx != gctx) d = d->next;
    if (!d && create) {
        d = calloc(1, sizeof *d);
        if (d) { d->gctx = gctx; d->next = g_decs; g_decs = d; }
    }
    pthread_mutex_unlock(&g_mu);
    return d;
}

static void frame_free(vdec *d) {
    for (int i = 0; i < NBUF; i++) if (d->gbuf[i]) { tm_gfree(d->gbuf[i]); d->gbuf[i] = 0; }
    d->w = d->h = 0;
}

/* Guest frame memory with libvpx's geometry: planes inside a bordered allocation. */
static int frame_alloc(vdec *d, uint32_t w, uint32_t h) {
    uint32_t aw = (w + 15u) & ~15u, ah = (h + 15u) & ~15u;
    uint32_t ys = aw + 2 * BORDER, uvs = ys / 2;
    uint32_t ysize = (ah + 2 * BORDER) * ys, uvsize = (ah / 2 + BORDER) * uvs;
    frame_free(d);
    for (int i = 0; i < NBUF; i++) {
        d->gbuf[i] = tm_heap_alloc(tm_heap_process(), 0, ysize + 2 * uvsize + 64);
        if (!d->gbuf[i]) { frame_free(d); return 0; }
        memset(G2H(d->gbuf[i]), 0, ysize);
        memset((uint8_t *)G2H(d->gbuf[i]) + ysize, 128, 2 * uvsize);
    }
    d->w = w; d->h = h; d->y_stride = ys; d->uv_stride = uvs;
    d->y_off = BORDER * ys + BORDER;
    d->u_off = ysize + (BORDER / 2) * uvs + BORDER / 2;
    d->v_off = ysize + uvsize + (BORDER / 2) * uvs + BORDER / 2;
    return 1;
}

/* One plane into guest memory; the alignment padding repeats the edge. */
static void plane_copy(uint8_t *dst, uint32_t dstride, const uint8_t *src, int sstride, uint32_t w, uint32_t h, uint32_t aw, uint32_t ah) {
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * dstride;
        memcpy(row, src + (ptrdiff_t)y * sstride, w);
        if (aw > w) memset(row + w, row[w - 1], aw - w);
    }
    for (uint32_t y = h; y < ah; y++) memcpy(dst + (size_t)y * dstride, dst + (size_t)(h - 1) * dstride, aw);
}

static void frame_copy(vdec *d, const vpx_image_t *img) {
    d->cur = (d->cur + 1) % NBUF;
    uint8_t *base = G2H(d->gbuf[d->cur]);
    uint32_t w = d->w, h = d->h, aw = (w + 15u) & ~15u, ah = (h + 15u) & ~15u;
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    plane_copy(base + d->y_off, d->y_stride, img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y], w, h, aw, ah);
    plane_copy(base + d->u_off, d->uv_stride, img->planes[VPX_PLANE_U], img->stride[VPX_PLANE_U], cw, ch, aw / 2, ah / 2);
    plane_copy(base + d->v_off, d->uv_stride, img->planes[VPX_PLANE_V], img->stride[VPX_PLANE_V], cw, ch, aw / 2, ah / 2);
}

/* vpx_codec_err_t decode(alg_priv, data, data_sz, user_priv, deadline) */
static void hook_vp8_decode(void) {
    uint32_t gctx = ARG(0), data = ARG(1), size = ARG(2), user = ARG(3);
    vdec *d = dec_find(gctx, 1);
    if (!d) RETC(VPX_CODEC_MEM_ERROR);
    if (!d->inited && !d->failed) {
        vpx_codec_dec_cfg_t cfg; memset(&cfg, 0, sizeof cfg);
        cfg.threads = (unsigned)g_threads;
        if (vpx_codec_dec_init(&d->codec, vpx_codec_vp8_dx(), &cfg, 0)) {
            d->failed = 1;
            TM_ERR("video: native VP8 decoder did not initialise: %s", vpx_codec_error(&d->codec));
        } else d->inited = 1;
    }
    if (!d->inited) RETC(VPX_CODEC_ERROR);
    wr32(gctx + CTX_USER_PRIV, user);
    d->have_frame = 0;
    if (!data || !size) {                        /* flush */
        vpx_codec_decode(&d->codec, NULL, 0, NULL, 0);
        RETC(VPX_CODEC_OK);
    }
    const uint8_t *p = G2H(data);
    /* A key frame carries the frame size: the guest buffer is (re)made before the
     * machine lock goes, the heap is not ours to use without it. */
    int kf = size >= 10 && !(p[0] & 1) && p[3] == 0x9D && p[4] == 0x01 && p[5] == 0x2A;
    if (kf) {
        uint32_t w = (p[6] | (uint32_t)p[7] << 8) & 0x3FFF, h = (p[8] | (uint32_t)p[9] << 8) & 0x3FFF;
        if (w && h && (w != d->w || h != d->h || !d->gbuf[0])) frame_alloc(d, w, h);
        wr32(gctx + CTX_SI_W, w); wr32(gctx + CTX_SI_H, h);
    }
    wr32(gctx + CTX_SI_KF, (uint32_t)kf);

    tm_machine_unlock();
    uint64_t t0 = tm_now_ns();
    vpx_codec_err_t res = vpx_codec_decode(&d->codec, p, size, NULL, 0);
    const vpx_image_t *img = NULL;
    int resize = 0;
    if (!res) {
        vpx_codec_iter_t it = NULL;
        img = vpx_codec_get_frame(&d->codec, &it);
        if (img) {
            if (d->gbuf[0] && img->d_w == d->w && img->d_h == d->h) { frame_copy(d, img); d->have_frame = 1; }
            else resize = 1;
        }
    }
    uint64_t dt = tm_now_ns() - t0;
    tm_machine_lock();
    if (resize && frame_alloc(d, img->d_w, img->d_h)) {   /* size without a key frame header we understood */
        frame_copy(d, img); d->have_frame = 1;
        wr32(gctx + CTX_SI_W, d->w); wr32(gctx + CTX_SI_H, d->h);
    }
    d->frames++; d->ns += dt;
    if (g_log) {
        d->log_frames++; d->log_ns += dt;
        uint64_t now = t0 + dt;
        if (!d->log_t0) d->log_t0 = now;
        else if (now - d->log_t0 >= 2000000000ull) {
            TM_INFO("video %08X: %ux%u, %.1f frames/s, %.2f ms per frame", gctx, d->w, d->h,
                    d->log_frames * 1e9 / (double)(now - d->log_t0), d->log_ns / 1e6 / (double)d->log_frames);
            d->log_frames = 0; d->log_ns = 0; d->log_t0 = now;
        }
    }
    if (res && g_log) TM_WARN("video %08X: decode error %d (%s), %u bytes", gctx, (int)res, vpx_codec_error(&d->codec), size);
    RETC((uint32_t)res);
}

/* vpx_image_t *get_frame(alg_priv, vpx_codec_iter_t *iter): one image per decode */
static void hook_vp8_get_frame(void) {
    uint32_t gctx = ARG(0), iter = ARG(1);
    vdec *d = dec_find(gctx, 0);
    if (!d || !iter || rd32(iter) || !d->have_frame || !d->gbuf[0]) RETC(0);
    d->have_frame = 0;
    uint32_t img = gctx + CTX_IMG, ah = (d->h + 15u) & ~15u, buf = d->gbuf[d->cur];
    wr32(img + IMG_FMT, 0x102);                      /* VPX_IMG_FMT_I420 */
    wr32(img + IMG_CS, 0); wr32(img + IMG_RANGE, 0);
    wr32(img + IMG_W, d->y_stride);
    wr32(img + IMG_H, ah + 2 * BORDER);
    wr32(img + IMG_BIT_DEPTH, 8);
    wr32(img + IMG_D_W, d->w); wr32(img + IMG_D_H, d->h);
    wr32(img + IMG_R_W, d->w); wr32(img + IMG_R_H, d->h);
    wr32(img + IMG_X_SHIFT, 1); wr32(img + IMG_Y_SHIFT, 1);
    wr32(img + IMG_PLANES + 0, buf + d->y_off);
    wr32(img + IMG_PLANES + 4, buf + d->u_off);
    wr32(img + IMG_PLANES + 8, buf + d->v_off);
    wr32(img + IMG_PLANES + 12, 0);
    wr32(img + IMG_STRIDE + 0, d->y_stride);
    wr32(img + IMG_STRIDE + 4, d->uv_stride);
    wr32(img + IMG_STRIDE + 8, d->uv_stride);
    wr32(img + IMG_STRIDE + 12, d->y_stride);
    wr32(img + IMG_BPS, 12);
    wr32(img + IMG_USER_PRIV, rd32(gctx + CTX_USER_PRIV));
    wr32(img + IMG_DATA, buf);
    wr32(img + IMG_DATA_OWNER, 0); wr32(img + IMG_SELF_ALLOCD, 0); wr32(img + IMG_FB_PRIV, 0);
    wr32(iter, img);
    RETC(img);
}

/* vpx_codec_err_t destroy(alg_priv) */
static void hook_vp8_destroy(void) {
    uint32_t gctx = ARG(0);
    pthread_mutex_lock(&g_mu);
    vdec **pp = &g_decs, *d = NULL;
    while (*pp && (*pp)->gctx != gctx) pp = &(*pp)->next;
    if (*pp) { d = *pp; *pp = d->next; }
    pthread_mutex_unlock(&g_mu);
    if (d) {
        if (g_log && d->frames) TM_INFO("video %08X closed: %ux%u, %llu frames, %.2f ms per frame", gctx, d->w, d->h,
                                        (unsigned long long)d->frames, d->ns / 1e6 / (double)d->frames);
        if (d->inited) vpx_codec_destroy(&d->codec);
        frame_free(d);
        free(d);
    }
    g_orig_destroy();                            /* the game's own: frees the guest object, returns to the caller */
}

int tm_video_native(void) {
    if (g_native < 0) {
        const char *e = getenv("TM_VIDEO_NATIVE"), *t = getenv("TM_VIDEO_THREADS"), *l = getenv("TM_VIDEO_LOG");
        g_native = !(e && *e == '0');
        if (t && *t) { g_threads = atoi(t); if (g_threads < 1) g_threads = 1; if (g_threads > 8) g_threads = 8; }
        g_log = l && *l && *l != '0';
    }
    return g_native;
}

/* Called once the image is mapped, before tm_parallel_register. */
void tm_video_register(void) {
    if (!tm_video_native()) { TM_INFO("video: the game's own VP8 decoder (TM_VIDEO_NATIVE=0)"); return; }
    g_orig_destroy = recomp_lookup(VA_VP8_DESTROY);
    if (!g_orig_destroy || !recomp_lookup(VA_VP8_DECODE) || !recomp_lookup(VA_VP8_GET_FRAME)) {
        TM_WARN("video: VP8 interface functions not found, keeping the game's decoder");
        g_native = 0; return;
    }
    tm_add_override(VA_VP8_DECODE, hook_vp8_decode);
    tm_add_override(VA_VP8_GET_FRAME, hook_vp8_get_frame);
    tm_add_override(VA_VP8_DESTROY, hook_vp8_destroy);
    TM_INFO("video: native VP8 decoder (%s), %d thread(s) per stream", vpx_codec_iface_name(vpx_codec_vp8_dx()), g_threads);
}

#else  /* no native libvpx in this build */
int tm_video_native(void) { return 0; }
void tm_video_register(void) {}
#endif
