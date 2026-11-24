/*
 * input.c - platform-neutral input state.
 *
 * The platform layer (Android activity, host test script) reports a gamepad
 * and keyboard keys here; XInput, DirectInput8 (dinput8.c) and the window
 * message queue (user32.c) read from it. Keys are reported as Windows virtual
 * keys and translated to DirectInput scan codes.
 *
 * Host testing: TM_INPUT_SCRIPT="sec:pad:A,sec:key:RETURN[:hold_ms],sec:click:x:y,..." presses
 * the named control at the given time (seconds after start) for hold_ms
 * (default 250); click = mouse move + left click at x,y in 1/1000 of the window.
 * A script with any pad entry reports a connected controller.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

void tm_user32_set_key(int vk, int down);
void tm_user32_post_input(uint32_t msg, uint32_t wp, uint32_t lp);

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- gamepads (XInput layout), up to 4: XInput user index 0..3 ----
 * The platform layer gives every physical controller its own slot (the built-in pad of a
 * handheld, or the on-screen touch pad, is normally slot 0). Local multiplayer (split
 * screen, Double Driver) reads the other slots. */
#define PADS 4
static struct { int connected; uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } g_pads[PADS];
/* A button stays reported for at least PAD_MIN_HOLD_NS after it went down, so a
 * tap shorter than one game frame (injected events, quick fingers) is not lost
 * between two polls. */
#define PAD_MIN_HOLD_NS 90000000ull
static uint64_t g_pad_down_ns[PADS][16];

/* ---- pad macro (diagnostics) ----
 * A timed sequence of controls that starts the moment the B button (restart race) goes
 * down, so a test drive has frame-accurate timing instead of the jitter of injected
 * events: TM_PAD_MACRO or "setprop debug.tm.padmacro" = "GAS:500:26000,LEFT:7300:8000"
 * (name:from_ms:to_ms after the press; GAS, BRAKE and the pad_names below). The property
 * is read again at every B press. */
#define MACRO_MAX 16
static struct { uint32_t mask; int trig; uint32_t from, to; } g_macro[MACRO_MAX];
static int g_macro_n; static uint64_t g_macro_t0; static uint32_t g_macro_last, g_macro_end;
static void macro_parse(const char *s);
static void macro_arm(uint64_t now) {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.padmacro", v) > 0) macro_parse(v); else if (!getenv("TM_PAD_MACRO")) g_macro_n = 0;
#endif
    if (g_macro_n) { g_macro_t0 = now; g_macro_last = 0; TM_INFO("input: pad macro started (%d steps, %u ms)", g_macro_n, g_macro_end); }
}

void tm_input_pad_slot(int i, int connected, uint32_t buttons, int lt, int rt, int lx, int ly, int rx, int ry) {
    if (i < 0 || i >= PADS) return;
    pthread_mutex_lock(&g_mu);
    if (g_pads[i].connected != connected) TM_INFO("input: gamepad %d %s", i, connected ? "connected" : "disconnected");
    g_pads[i].connected = connected;
    uint64_t now = tm_now_ns();
    if (i == 0 && (buttons & 0x2000) && !(g_pads[0].buttons & 0x2000)) macro_arm(now);
    for (int b = 0; b < 16; b++) if ((buttons & (1u << b)) && !(g_pads[i].buttons & (1u << b))) g_pad_down_ns[i][b] = now;
    if (!connected) { buttons = 0; lt = rt = lx = ly = rx = ry = 0; memset(g_pad_down_ns[i], 0, sizeof g_pad_down_ns[i]); }
    g_pads[i].buttons = (uint16_t)buttons; g_pads[i].lt = (uint8_t)lt; g_pads[i].rt = (uint8_t)rt;
    g_pads[i].lx = (int16_t)lx; g_pads[i].ly = (int16_t)ly; g_pads[i].rx = (int16_t)rx; g_pads[i].ry = (int16_t)ry;
    g_pads[i].packet++;
    pthread_mutex_unlock(&g_mu);
}
void tm_input_pad(int connected, uint32_t buttons, int lt, int rt, int lx, int ly, int rx, int ry) {
    tm_input_pad_slot(0, connected, buttons, lt, rt, lx, ly, rx, ry);
}

/* Fills an XINPUT_STATE (16 bytes) at host pointer p for pad i; returns 0 if not connected. */
int tm_input_pad_get_slot(int i, uint8_t *p) {
    if (i < 0 || i >= PADS) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = g_pads[i].connected;
    if (ok) {
        uint16_t buttons = g_pads[i].buttons; uint32_t packet = g_pads[i].packet;
        uint64_t now = tm_now_ns();
        for (int b = 0; b < 16; b++)
            if (g_pad_down_ns[i][b] && now - g_pad_down_ns[i][b] < PAD_MIN_HOLD_NS) buttons |= (uint16_t)(1u << b);
            else if (g_pad_down_ns[i][b] && !(g_pads[i].buttons & (1u << b))) { g_pad_down_ns[i][b] = 0; packet++; g_pads[i].packet = packet; }
        uint8_t lt = g_pads[i].lt, rt = g_pads[i].rt;
        if (i == 0 && g_macro_t0) {
            uint32_t e = (uint32_t)((now - g_macro_t0) / 1000000ull), state = 0;
            for (int k = 0; k < g_macro_n; k++) if (e >= g_macro[k].from && e < g_macro[k].to) {
                buttons |= (uint16_t)g_macro[k].mask; state |= 1u << k;
                if (g_macro[k].trig == 1) rt = 255; else if (g_macro[k].trig == 2) lt = 255;
            }
            if (state != g_macro_last) { g_macro_last = state; packet = ++g_pads[0].packet; }
            if (e > g_macro_end) { g_macro_t0 = 0; TM_INFO("input: pad macro done"); }
        }
        memcpy(p, &packet, 4); memcpy(p + 4, &buttons, 2);
        p[6] = lt; p[7] = rt;
        memcpy(p + 8, &g_pads[i].lx, 2); memcpy(p + 10, &g_pads[i].ly, 2); memcpy(p + 12, &g_pads[i].rx, 2); memcpy(p + 14, &g_pads[i].ry, 2);
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int tm_input_pad_get(uint8_t *p) { return tm_input_pad_get_slot(0, p); }

/* Vibration (XInputSetState): the platform layer registers a sink; it is called only when
 * a pad's motor speeds change. */
void (*tm_rumble_sink)(int pad, int low, int high);
static uint16_t g_rumble[PADS][2];
void tm_input_rumble(int i, int low, int high) {
    if (i < 0 || i >= PADS) return;
    if (g_rumble[i][0] == low && g_rumble[i][1] == high) return;
    g_rumble[i][0] = (uint16_t)low; g_rumble[i][1] = (uint16_t)high;
    if (tm_rumble_sink) tm_rumble_sink(i, low, high);
}

/* ---- mouse (touch screen, or a real mouse) ----
 * The platform reports an absolute position (0..1 over the game surface), the buttons and
 * wheel steps. The game sees it as the Windows cursor -- WM_MOUSEMOVE / WM_xBUTTONDOWN/UP /
 * WM_MOUSEWHEEL in client coordinates of its window -- and as the DirectInput mouse
 * (relative motion and buttons, immediate and buffered). The ManiaPlanet overlay bar at the
 * top edge of the screen (graphics, sound, input settings) opens on the cursor. */
void tm_user32_client_size(int *w, int *h);
static struct { int x, y, buttons, have; int dx, dy, dz; } g_mouse;
#define MEVQ 128
static struct { uint8_t ofs, pad[3]; uint32_t data, time, seq; } g_mev[MEVQ];
static unsigned g_mev_h, g_mev_t; static uint32_t g_mseq;
static void mev_push(uint8_t ofs, uint32_t data) {          /* under g_mu */
    unsigned n = (g_mev_t + 1) % MEVQ;
    if (n == g_mev_h) return;
    g_mev[g_mev_t].ofs = ofs; g_mev[g_mev_t].data = data; g_mev[g_mev_t].time = tm_guest_tick_ms(); g_mev[g_mev_t].seq = ++g_mseq;
    g_mev_t = n;
}
static int g_home = 1;                                       /* TM_MOUSE_HOME=0: no corner homing */
static uint32_t mk_flags(int b) { return (b & 1 ? 0x0001u : 0) | (b & 2 ? 0x0002u : 0) | (b & 4 ? 0x0010u : 0); }

void tm_input_mouse(float nx, float ny, int buttons, int wheel) {
    static int init;
    if (!init) { init = 1; const char *e = getenv("TM_MOUSE_HOME"); g_home = !(e && *e == '0'); }
    int cw = 0, ch = 0;
    tm_user32_client_size(&cw, &ch);
    if (cw <= 0 || ch <= 0) return;
    if (nx < 0) nx = 0; if (nx > 1) nx = 1; if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    int x = (int)(nx * (float)(cw - 1) + 0.5f), y = (int)(ny * (float)(ch - 1) + 0.5f);
    pthread_mutex_lock(&g_mu);
    int moved = !g_mouse.have || x != g_mouse.x || y != g_mouse.y;
    int dx = g_mouse.have ? x - g_mouse.x : 0, dy = g_mouse.have ? y - g_mouse.y : 0;
    int old = g_mouse.buttons;
    /* The game hides the Windows cursor and moves its own by the DirectInput motion, from
     * a start of its choosing. When the cursor jumps (a new touch; and the very first time)
     * the motion first runs into the top-left corner -- the game keeps its cursor inside
     * the window -- and from there to the touch, so its cursor lands where the finger is. */
    if (!g_mouse.have || (!old && !buttons && (dx * dx + dy * dy) > 40 * 40)) {   /* a jump (a new touch) */
        if (g_home) { mev_push(0, (uint32_t)-20000); mev_push(4, (uint32_t)-20000); g_mouse.dx -= 20000; g_mouse.dy -= 20000; }
        dx = x; dy = y;
    }
    g_mouse.x = x; g_mouse.y = y; g_mouse.have = 1; g_mouse.buttons = buttons;
    g_mouse.dx += dx; g_mouse.dy += dy; g_mouse.dz += wheel * 120;
    if (dx) mev_push(0, (uint32_t)dx);
    if (dy) mev_push(4, (uint32_t)dy);
    if (wheel) mev_push(8, (uint32_t)(wheel * 120));
    for (int b = 0; b < 3; b++) if ((old ^ buttons) & (1 << b)) mev_push((uint8_t)(12 + b), buttons & (1 << b) ? 0x80 : 0);
    pthread_mutex_unlock(&g_mu);
    for (int b = 0; b < 3; b++) if ((old ^ buttons) & (1 << b))                       /* GetAsyncKeyState(VK_xBUTTON) */
        tm_user32_set_key(b == 0 ? 0x01 : b == 1 ? 0x02 : 0x04, buttons & (1 << b));
    uint32_t lp = ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFF);
    if (moved) tm_user32_post_input(0x0200, mk_flags(buttons), lp);                 /* WM_MOUSEMOVE */
    static const uint32_t down_msg[3] = { 0x0201, 0x0204, 0x0207 }, up_msg[3] = { 0x0202, 0x0205, 0x0208 };
    for (int b = 0; b < 3; b++) if ((old ^ buttons) & (1 << b))
        tm_user32_post_input(buttons & (1 << b) ? down_msg[b] : up_msg[b], mk_flags(buttons), lp);
    if (wheel) tm_user32_post_input(0x020A, ((uint32_t)(uint16_t)(int16_t)(wheel * 120) << 16) | mk_flags(buttons), lp);   /* WM_MOUSEWHEEL */
}

/* The game put the Windows cursor somewhere (SetCursorPos, screen = client coordinates
 * here): relative motion is measured from there on. */
void tm_input_mouse_warp(int x, int y) {
    pthread_mutex_lock(&g_mu);
    g_mouse.x = x; g_mouse.y = y; g_mouse.have = 1;
    pthread_mutex_unlock(&g_mu);
}

/* DirectInput mouse: DIMOUSESTATE (16 bytes) / DIMOUSESTATE2 (20 bytes), motion since the last call. */
void tm_input_mouse_state(uint8_t *out, uint32_t cb) {
    pthread_mutex_lock(&g_mu);
    int32_t v[3] = { g_mouse.dx, g_mouse.dy, g_mouse.dz };
    g_mouse.dx = g_mouse.dy = g_mouse.dz = 0;
    int b = g_mouse.buttons;
    pthread_mutex_unlock(&g_mu);
    if (cb >= 12) memcpy(out, v, 12);
    for (uint32_t i = 0; i < 8 && 12 + i < cb; i++) out[12 + i] = (i < 3 && (b & (1 << i))) ? 0x80 : 0;
}
int tm_input_mouse_events(uint32_t *ofs, uint32_t *data, uint32_t *time, uint32_t *seq, int max, int remove) {
    pthread_mutex_lock(&g_mu);
    int n = 0; unsigned h = g_mev_h;
    while (h != g_mev_t && n < max) {
        if (ofs) { ofs[n] = g_mev[h].ofs; data[n] = g_mev[h].data; time[n] = g_mev[h].time; seq[n] = g_mev[h].seq; }
        n++; h = (h + 1) % MEVQ;
    }
    if (remove) g_mev_h = h;
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* ---- keyboard ---- */
static uint8_t g_dik[256];                       /* DirectInput key state, 0x80 = down */
#define EVQ 256
static struct { uint8_t dik, down; uint32_t time, seq; } g_ev[EVQ];
static unsigned g_ev_h, g_ev_t; static uint32_t g_seq;

static int vk_to_dik(int vk) {
    static const uint8_t row_q[] = "QWERTYUIOP", row_a[] = "ASDFGHJKL", row_z[] = "ZXCVBNM";
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 'A' && vk <= 'Z') {
        for (int i = 0; row_q[i]; i++) if (row_q[i] == vk) return 0x10 + i;
        for (int i = 0; row_a[i]; i++) if (row_a[i] == vk) return 0x1E + i;
        for (int i = 0; row_z[i]; i++) if (row_z[i] == vk) return 0x2C + i;
    }
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);   /* F1..F10 */
    switch (vk) {
    case 0x1B: return 0x01;  case 0x08: return 0x0E;  case 0x09: return 0x0F;  case 0x0D: return 0x1C;
    case 0x20: return 0x39;  case 0x10: return 0x2A;  case 0x11: return 0x1D;  case 0x12: return 0x38;
    case 0x25: return 0xCB;  case 0x26: return 0xC8;  case 0x27: return 0xCD;  case 0x28: return 0xD0;
    case 0x2E: return 0xD3;  case 0x2D: return 0xD2;  case 0x24: return 0xC7;  case 0x23: return 0xCF;
    case 0x21: return 0xC9;  case 0x22: return 0xD1;  case 0x7A: return 0x57;  case 0x7B: return 0x58;
    }
    return 0;
}

__attribute__((weak)) void tm_gfx_debug_cycle(void);       /* gfx/d3d11_bridge.cpp */
__attribute__((weak)) void tm_gfx_trace_request(void);
__attribute__((weak)) void tm_gfx_capture_deferred_request(void);
__attribute__((weak)) void tm_gfx_capture_request(void);   /* gfx/d3d11_bridge.cpp */

/* A key stays down for at least KEY_MIN_HOLD_NS: the game samples the keyboard
 * once per frame, and a tap that goes down and up between two samples (injected
 * events do) would otherwise only ever be seen as "up". The release is applied
 * by the next poll after the hold time. */
#define KEY_MIN_HOLD_NS 90000000ull
static uint64_t g_key_down_ns[256];
static uint8_t g_key_up_pending[256];
static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER;
static void key_apply(int vk, int down);

static void key_flush_releases(void) {
    int due[16], n = 0;
    uint64_t now = tm_now_ns();
    pthread_mutex_lock(&g_hold_mu);
    for (int vk = 0; vk < 256 && n < 16; vk++)
        if (g_key_up_pending[vk] && now - g_key_down_ns[vk] >= KEY_MIN_HOLD_NS) { g_key_up_pending[vk] = 0; due[n++] = vk; }
    pthread_mutex_unlock(&g_hold_mu);
    for (int i = 0; i < n; i++) key_apply(due[i], 0);
}

void tm_input_key(int vk, int down) {
    if (vk == 0x7B) {                               /* F12: dump one frame's render targets (diagnostics) */
        if (down && tm_gfx_capture_request) tm_gfx_capture_request();
        return;
    }
    if (vk == 0x77) {                               /* F8: capture one frame without waiting inside it (diagnostics) */
        if (down && tm_gfx_capture_deferred_request) tm_gfx_capture_deferred_request();
        return;
    }
    if (vk == 0x78) {                               /* F9: next TM_GFX_SYNC mode (diagnostics) */
        if (down && tm_gfx_debug_cycle) tm_gfx_debug_cycle();
        return;
    }
    if (vk == 0x79) {                               /* F10: trace one frame's resource traffic (diagnostics) */
        if (down && tm_gfx_trace_request) tm_gfx_trace_request();
        return;
    }
    if (vk == 0x7A) {                               /* F11: dump guest memory (diagnostics) */
        if (down) tm_memdump_request();
        return;
    }
    if (vk < 0 || vk > 255) return;
    key_flush_releases();
    pthread_mutex_lock(&g_hold_mu);
    int defer = 0;
    if (down) { g_key_down_ns[vk] = tm_now_ns(); g_key_up_pending[vk] = 0; }
    else if (tm_now_ns() - g_key_down_ns[vk] < KEY_MIN_HOLD_NS) { g_key_up_pending[vk] = 1; defer = 1; }
    pthread_mutex_unlock(&g_hold_mu);
    if (!defer) key_apply(vk, down);
}

static void key_apply(int vk, int down) {
    int dik = vk_to_dik(vk);
    pthread_mutex_lock(&g_mu);
    if (dik) {
        g_dik[dik] = down ? 0x80 : 0;
        unsigned n = (g_ev_t + 1) % EVQ;
        if (n != g_ev_h) {
            g_ev[g_ev_t].dik = (uint8_t)dik; g_ev[g_ev_t].down = (uint8_t)(down != 0);
            g_ev[g_ev_t].time = tm_guest_tick_ms(); g_ev[g_ev_t].seq = ++g_seq;
            g_ev_t = n;
        }
    }
    pthread_mutex_unlock(&g_mu);
    tm_user32_set_key(vk, down);
    uint32_t lp = 1u | ((uint32_t)(dik & 0x7F) << 16) | (dik & 0x80 ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
    tm_user32_post_input(down ? 0x0100 : 0x0101, (uint32_t)vk, lp);            /* WM_KEYDOWN / WM_KEYUP */
    if (down && (vk == 0x0D || vk == 0x1B || vk == 0x20 || vk == 0x08 || (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')))
        tm_user32_post_input(0x0102, (uint32_t)(vk >= 'A' && vk <= 'Z' ? vk + 32 : vk), lp);   /* WM_CHAR */
}

void tm_input_kbd_state(uint8_t *out, uint32_t cb) {
    key_flush_releases();
    pthread_mutex_lock(&g_mu);
    memcpy(out, g_dik, cb < 256 ? cb : 256);
    pthread_mutex_unlock(&g_mu);
}

/* Buffered keyboard events for IDirectInputDevice8::GetDeviceData. Returns the
 * number of events copied (max); remove = consume them. */
int tm_input_kbd_events(uint32_t *dik, uint32_t *data, uint32_t *time, uint32_t *seq, int max, int remove) {
    key_flush_releases();
    pthread_mutex_lock(&g_mu);
    int n = 0; unsigned h = g_ev_h;
    while (h != g_ev_t && n < max) {
        if (dik) { dik[n] = g_ev[h].dik; data[n] = g_ev[h].down ? 0x80 : 0; time[n] = g_ev[h].time; seq[n] = g_ev[h].seq; }
        n++; h = (h + 1) % EVQ;
    }
    if (remove) g_ev_h = h;
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* ---- scripted input (host bring-up) ---- */
static const struct { const char *n; uint32_t v; } pad_names[] = {
    {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
    {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000} };
static const struct { const char *n; int vk; } key_names[] = {
    {"RETURN", 0x0D}, {"ENTER", 0x0D}, {"ESCAPE", 0x1B}, {"ESC", 0x1B}, {"SPACE", 0x20}, {"UP", 0x26}, {"DOWN", 0x28},
    {"LEFT", 0x25}, {"RIGHT", 0x27}, {"TAB", 0x09}, {"BACK", 0x08}, {"DELETE", 0x2E}, {"F11", 0x7A}, {"F12", 0x7B} };

static void macro_parse(const char *s) {
    g_macro_n = 0; g_macro_end = 0;
    char buf[128]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char *save = NULL;
    for (char *e = strtok_r(buf, ",", &save); e && g_macro_n < MACRO_MAX; e = strtok_r(NULL, ",", &save)) {
        char name[16] = ""; unsigned from = 0, to = 0;
        if (sscanf(e, " %15[^:]:%u:%u", name, &from, &to) < 3 || to <= from) continue;
        uint32_t mask = 0; int trig = !strcasecmp(name, "GAS") ? 1 : !strcasecmp(name, "BRAKE") ? 2 : 0;
        for (unsigned i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++) if (!strcasecmp(pad_names[i].n, name)) mask = pad_names[i].v;
        if (!mask && !trig) continue;
        g_macro[g_macro_n].mask = mask; g_macro[g_macro_n].trig = trig; g_macro[g_macro_n].from = from; g_macro[g_macro_n].to = to;
        if (to > g_macro_end) g_macro_end = to;
        g_macro_n++;
    }
}

static void *script_thread(void *arg) {
    char *s = arg, *save = NULL;
    uint64_t t0 = tm_now_ns();
    for (char *e = strtok_r(s, ",", &save); e; e = strtok_r(NULL, ",", &save)) {
        double at = 0; char dev[8] = "", name[16] = ""; int hold = 250;
        if (sscanf(e, "%lf:%7[^:]:%15[^:]:%d", &at, dev, name, &hold) < 3) { TM_WARN("input script: bad entry '%s'", e); continue; }
        int64_t wait = (int64_t)(at * 1e9) - (int64_t)(tm_now_ns() - t0);
        if (wait > 0) usleep((useconds_t)(wait / 1000));
        if (!strcasecmp(dev, "click")) {                /* sec:click:<x>:<y> in 1/1000 of the window */
            unsigned cx = 0, cy = 0;
            if (sscanf(e, "%*f:%*[^:]:%u:%u", &cx, &cy) == 2) {
                float nx = cx / 1000.0f, ny = cy / 1000.0f;
                TM_INFO("input script: click %.3f %.3f", nx, ny);
                tm_input_mouse(nx, ny, 0, 0); usleep(150000);
                tm_input_mouse(nx, ny, 1, 0); usleep(120000);
                tm_input_mouse(nx, ny, 0, 0);
            }
            continue;
        }
        if (!strcasecmp(dev, "pad")) {
            uint32_t b = 0;
            for (unsigned i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++) if (!strcasecmp(pad_names[i].n, name)) b = pad_names[i].v;
            TM_INFO("input script: pad %s", name);
            tm_input_pad(1, b, 0, 0, 0, 0, 0, 0); usleep((useconds_t)hold * 1000); tm_input_pad(1, 0, 0, 0, 0, 0, 0, 0);
        } else {
            int vk = strlen(name) == 1 ? (name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]) : 0;
            for (unsigned i = 0; i < sizeof key_names / sizeof key_names[0]; i++) if (!strcasecmp(key_names[i].n, name)) vk = key_names[i].vk;
            TM_INFO("input script: key %s (vk %02X)", name, vk);
            tm_input_key(vk, 1); usleep((useconds_t)hold * 1000); tm_input_key(vk, 0);
        }
    }
    return NULL;
}

/* Host testing: TM_INPUT_FILE=<path> - lines written to that file are run like script
   entries without the time ("click:x:y", "pad:A", "key:RETURN"), then the file is emptied. */
static void *input_file_thread(void *arg) {
    const char *path = arg;
    for (;;) {
        usleep(200000);
        FILE *f = fopen(path, "r+");
        if (!f) continue;
        char line[128]; int any = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            char entry[160]; snprintf(entry, sizeof entry, "0:%s", line);
            char *dup = strdup(entry);
            script_thread(dup);                      /* frees nothing: entries are tiny */
            any = 1;
        }
        if (any) { if (ftruncate(fileno(f), 0) != 0) { } }
        fclose(f);
    }
    return NULL;
}

void tm_input_init(void) {
    const char *fp = getenv("TM_INPUT_FILE");
    if (fp && *fp) { pthread_t ft; pthread_create(&ft, NULL, input_file_thread, strdup(fp)); pthread_detach(ft); }
    const char *m = getenv("TM_PAD_MACRO");
    if (m && *m) macro_parse(m);
    const char *s = getenv("TM_INPUT_SCRIPT");
    if (!s || !*s) return;
    if (strstr(s, ":pad:")) tm_input_pad(1, 0, 0, 0, 0, 0, 0, 0);
    pthread_t t; pthread_create(&t, NULL, script_thread, strdup(s)); pthread_detach(t);
}
