/*
 * user32.c - a minimal, honest window system for one full-screen game window.
 *
 * Windows are host records with a guest HWND value; window procedures are
 * guest code called through tm_call_guest. The message queue is per process
 * (the game pumps on its main thread). The platform layer posts input as
 * WM_* messages through tm_user32_post_input().
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_GETMINMAXINFO 0x0024
#define WM_NCCREATE 0x0081
#define WM_NCCALCSIZE 0x0083
#define WM_TIMER 0x0113
#define WM_QUIT 0x0012
#define WM_SETTEXT 0x000C
#define WM_WINDOWPOSCHANGED 0x0047

typedef struct {
    uint32_t hwnd, wndproc, style, exstyle, instance, userdata, parent;
    int x, y, w, h;
    int visible;
    uint32_t class_atom;
    uint32_t longs[16];
} tm_window;

typedef struct { uint32_t atom; char name[128]; uint32_t wndproc, style, extra, instance, icon, cursor, bg, menu, extra_wnd; } tm_wclass;

#define MAX_WIN 64
static tm_window g_win[MAX_WIN];
static tm_wclass g_cls[64];
static int g_ncls;
static uint32_t g_focus, g_foreground;
static int g_screen_w = 1920, g_screen_h = 1080;

typedef struct { uint32_t hwnd, msg, wparam, lparam, time; int32_t x, y; } tm_msg;
#define QSIZE 1024
static tm_msg g_q[QSIZE];
static int g_qh, g_qt;
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_quit_posted; static uint32_t g_quit_code;

static tm_window *win_get(uint32_t h) {
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].hwnd && g_win[i].hwnd == h) return &g_win[i];
    return NULL;
}

void tm_user32_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }
/* Fill mode: the platform shows exactly one full-screen surface (Android), so a
 * top-level window always covers the screen - as if maximized without borders -
 * whatever size the game asks for. The game learns the size through WM_SIZE /
 * GetClientRect and sizes its swap chain to it. */
static int g_fill = -1;
void tm_user32_set_fill(int on) { g_fill = on; }
static int fill_mode(void) {
    if (g_fill < 0) { const char *e = getenv("TM_WINDOW_FILL"); g_fill = e && *e && *e != '0'; }
    return g_fill;
}

static void q_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_q_mu);
    int n = (g_qt + 1) % QSIZE;
    if (n != g_qh) { g_q[g_qt] = (tm_msg){ hwnd, msg, wp, lp, tm_guest_tick_ms(), 0, 0 }; g_qt = n; }
    pthread_mutex_unlock(&g_q_mu);
}
void tm_user32_client_size(int *w, int *h) {
    uint32_t hw = g_focus ? g_focus : g_win[0].hwnd;
    tm_window *win = win_get(hw);
    if (win) { *w = (int)win->w; *h = (int)win->h; } else { *w = g_screen_w; *h = g_screen_h; }
}
void tm_user32_post_input(uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t h = g_focus ? g_focus : (g_win[0].hwnd);
    q_post(h, msg, wp, lp);
}

static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    tm_window *w = win_get(hwnd);
    if (!w || !w->wndproc) return 0;
    TM_TRACE("SendMessage %08X msg %04X wp %08X lp %08X", hwnd, msg, wp, lp);
    return tm_call_guest(w->wndproc, 4, hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------ classes */
static void shim_RegisterClassExW(void) {
    uint32_t c = ARG(0);
    if (g_ncls >= 64) RET(0, 1);
    tm_wclass *k = &g_cls[g_ncls];
    k->style = rd32(c + 4); k->wndproc = rd32(c + 8); k->extra = rd32(c + 12); k->extra_wnd = rd32(c + 16);
    k->instance = rd32(c + 20); k->icon = rd32(c + 24); k->cursor = rd32(c + 28);
    uint32_t name = rd32(c + 40);
    if (name < 0x10000) snprintf(k->name, sizeof k->name, "#%u", name);
    else tm_w2u(name, k->name, sizeof k->name);
    k->atom = 0xC000 + (uint32_t)g_ncls;
    g_ncls++;
    TM_INFO("RegisterClassExW(%s, wndproc %08X) -> %04X", k->name, k->wndproc, k->atom);
    RET(k->atom, 1);
}
static void shim_UnregisterClassW(void) { RET(1, 2); }

static tm_wclass *cls_find(uint32_t name) {
    char n[128];
    if (name < 0x10000) { for (int i = 0; i < g_ncls; i++) if (g_cls[i].atom == name) return &g_cls[i]; return NULL; }
    tm_w2u(name, n, sizeof n);
    for (int i = 0; i < g_ncls; i++) if (!strcasecmp(g_cls[i].name, n)) return &g_cls[i];
    return NULL;
}

/* ------------------------------------------------------------ windows */
static void shim_CreateWindowExW(void) {
    uint32_t ex = ARG(0), clsname = ARG(1), title = ARG(2), style = ARG(3);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    uint32_t parent = ARG(8), menu = ARG(9), inst = ARG(10), param = ARG(11);
    tm_wclass *k = cls_find(clsname);
    if (!k) { TM_WARN("CreateWindowExW: unknown class"); tm_set_last_error(1407); RET(0, 12); }
    int slot = -1;
    for (int i = 0; i < MAX_WIN; i++) if (!g_win[i].hwnd) { slot = i; break; }
    if (slot < 0) RET(0, 12);
    tm_window *wn = &g_win[slot];
    memset(wn, 0, sizeof *wn);
    wn->hwnd = 0x00010010u + (uint32_t)slot * 0x10u;
    wn->wndproc = k->wndproc; wn->style = style; wn->exstyle = ex; wn->instance = inst; wn->parent = parent;
    if (x == (int)0x80000000) { x = 0; y = 0; }
    if (w == (int)0x80000000) { w = g_screen_w; h = g_screen_h; }
    if (fill_mode() && !parent) { x = 0; y = 0; w = g_screen_w; h = g_screen_h; }
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->class_atom = k->atom;
    char t[256]; tm_w2u(title, t, sizeof t);
    TM_INFO("CreateWindowExW('%s', class %s, style %08X, %d,%d %dx%d) -> %08X", t, k->name, style, x, y, w, h, wn->hwnd);
    /* CREATESTRUCTW on the guest stack area: lpCreateParams, hInstance, hMenu,
     * hwndParent, cy, cx, y, x, style, lpszName, lpszClass, dwExStyle */
    uint32_t cs = tm_galloc(48);
    wr32(cs + 0, param); wr32(cs + 4, inst); wr32(cs + 8, menu); wr32(cs + 12, parent);
    wr32(cs + 16, (uint32_t)h); wr32(cs + 20, (uint32_t)w); wr32(cs + 24, (uint32_t)y); wr32(cs + 28, (uint32_t)x);
    wr32(cs + 32, style); wr32(cs + 36, title); wr32(cs + 40, clsname); wr32(cs + 44, ex);
    uint32_t hwnd = wn->hwnd;
    if (!send_msg(hwnd, WM_NCCREATE, 0, cs)) { TM_WARN("WM_NCCREATE refused"); }
    if ((int32_t)send_msg(hwnd, WM_CREATE, 0, cs) == -1) { TM_WARN("WM_CREATE failed"); wn->hwnd = 0; tm_gfree(cs); RET(0, 12); }
    tm_gfree(cs);
    if (style & 0x10000000u) { wn->visible = 1; send_msg(hwnd, WM_SHOWWINDOW, 1, 0); }
    send_msg(hwnd, WM_SIZE, 0, ((uint32_t)h << 16) | ((uint32_t)w & 0xFFFF));
    if (g_platform.on_window_created) g_platform.on_window_created(hwnd, w, h);
    RET(hwnd, 12);
}
static void shim_DestroyWindow(void) {
    tm_window *w = win_get(ARG(0));
    if (w) { send_msg(w->hwnd, WM_DESTROY, 0, 0); w->hwnd = 0; }
    RET(w != NULL, 1);
}
static void shim_DefWindowProcW(void) {
    uint32_t msg = ARG(1);
    if (msg == WM_NCCREATE) RET(1, 4);
    RET(0, 4);
}
static void shim_ShowWindow(void) {
    tm_window *w = win_get(ARG(0));
    if (!w) RET(0, 2);
    int was = w->visible;
    w->visible = ARG(1) != 0;
    if (w->visible && !was) {
        send_msg(w->hwnd, WM_SHOWWINDOW, 1, 0);
        send_msg(w->hwnd, WM_ACTIVATEAPP, 1, 0);
        send_msg(w->hwnd, WM_ACTIVATE, 1, 0);
        g_focus = g_foreground = w->hwnd;
        send_msg(w->hwnd, WM_SETFOCUS, 0, 0);
        send_msg(w->hwnd, WM_SIZE, 0, ((uint32_t)w->h << 16) | ((uint32_t)w->w & 0xFFFF));
    }
    RET(was, 2);
}
static void shim_UpdateWindow(void) { RET(1, 1); }
static void shim_SetWindowPos(void) {
    tm_window *w = win_get(ARG(0));
    uint32_t flags = ARG(6);
    if (!w) RET(0, 7);
    int fill = fill_mode() && !w->parent;
    if (!(flags & 0x2) && !fill) { w->x = (int)ARG(2); w->y = (int)ARG(3); }
    if (!(flags & 0x1)) {
        if (!fill) { w->w = (int)ARG(4); w->h = (int)ARG(5); }
        send_msg(w->hwnd, WM_SIZE, fill ? 2 : 0, ((uint32_t)w->h << 16) | ((uint32_t)w->w & 0xFFFF));
    }
    if (flags & 0x40) w->visible = 1;
    if (flags & 0x80) w->visible = 0;
    RET(1, 7);
}
static void put_rect(uint32_t r, int l, int t, int rr, int b) { wr32(r, (uint32_t)l); wr32(r + 4, (uint32_t)t); wr32(r + 8, (uint32_t)rr); wr32(r + 12, (uint32_t)b); }
static void shim_GetClientRect(void) { tm_window *w = win_get(ARG(0)); if (!w) RET(0, 2); put_rect(ARG(1), 0, 0, w->w, w->h); RET(1, 2); }
static void shim_GetWindowRect(void) {
    tm_window *w = win_get(ARG(0));
    if (!w) { if (ARG(0) == 0x10000) { put_rect(ARG(1), 0, 0, g_screen_w, g_screen_h); RET(1, 2); } RET(0, 2); }
    put_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h); RET(1, 2);
}
static void shim_SetRect(void) { put_rect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); RET(1, 5); }
static void shim_GetDesktopWindow(void) { RET(0x10000, 0); }
static void shim_IsIconic(void) { RET(0, 1); }
static void shim_GetWindowLongW(void) {
    tm_window *w = win_get(ARG(0)); int32_t idx = (int32_t)ARG(1);
    if (!w) RET(0, 2);
    switch (idx) {
    case -4: RET(w->wndproc, 2);
    case -16: RET(w->style, 2);
    case -20: RET(w->exstyle, 2);
    case -21: RET(w->userdata, 2);
    case -6: RET(w->instance, 2);
    default: RET(idx >= 0 && idx < 64 ? w->longs[idx / 4] : 0, 2);
    }
}
static void shim_SetWindowLongW(void) {
    tm_window *w = win_get(ARG(0)); int32_t idx = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0;
    if (!w) RET(0, 3);
    switch (idx) {
    case -4: old = w->wndproc; w->wndproc = v; break;
    case -16: old = w->style; w->style = v; break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->userdata; w->userdata = v; break;
    default: if (idx >= 0 && idx < 64) { old = w->longs[idx / 4]; w->longs[idx / 4] = v; } break;
    }
    RET(old, 3);
}
static void shim_SetWindowTextW(void) { RET(1, 2); }
static void shim_SetFocus(void) { uint32_t old = g_focus; g_focus = ARG(0); RET(old, 1); }
static void shim_GetFocus(void) { RET(g_focus, 0); }
static void shim_SetForegroundWindow(void) { g_foreground = ARG(0); RET(1, 1); }
static void shim_GetForegroundWindow(void) { RET(g_foreground, 0); }
static void shim_BringWindowToTop(void) { RET(1, 1); }
static void shim_LockSetForegroundWindow(void) { RET(1, 1); }
static void shim_FindWindowExW(void) { RET(0, 4); }
static void shim_FlashWindowEx(void) { RET(0, 1); }
static void shim_SendMessageW(void) { RET(send_msg(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }

/* ------------------------------------------------------------ messages */
static int q_get(tm_msg *m, int remove) {
    pthread_mutex_lock(&g_q_mu);
    int ok = g_qh != g_qt;
    if (ok) { *m = g_q[g_qh]; if (remove) g_qh = (g_qh + 1) % QSIZE; }
    pthread_mutex_unlock(&g_q_mu);
    return ok;
}
static void put_msg(uint32_t p, tm_msg *m) {
    wr32(p, m->hwnd); wr32(p + 4, m->msg); wr32(p + 8, m->wparam); wr32(p + 12, m->lparam);
    wr32(p + 16, m->time); wr32(p + 20, (uint32_t)m->x); wr32(p + 24, (uint32_t)m->y);
}
static void (*g_pump_hook)(void);
void tm_user32_set_pump_hook(void (*f)(void)) { g_pump_hook = f; }

static void shim_PeekMessageW(void) {
    uint32_t out = ARG(0), flags = ARG(4);
    if (g_pump_hook) g_pump_hook();
    tm_msg m;
    if (g_quit_posted && !q_get(&m, 0)) {
        m = (tm_msg){ 0, WM_QUIT, g_quit_code, 0, 0, 0, 0 };
        put_msg(out, &m);
        if (flags & 1) g_quit_posted = 0;
        RET(1, 5);
    }
    if (!q_get(&m, flags & 1)) RET(0, 5);
    put_msg(out, &m);
    RET(1, 5);
}
static void shim_TranslateMessage(void) { RET(0, 1); }
static void shim_TranslateAcceleratorW(void) { RET(0, 3); }
static void shim_LoadAcceleratorsW(void) { RET(0x7700, 2); }
static void shim_DispatchMessageW(void) {
    uint32_t p = ARG(0);
    uint32_t hwnd = rd32(p), msg = rd32(p + 4), wp = rd32(p + 8), lp = rd32(p + 12);
    {
        static int mlog = -1;
        if (mlog < 0) { const char *e = getenv("TM_MOUSE_LOG"); mlog = e && *e && *e != '0'; }
        if (mlog && msg >= 0x0201 && msg <= 0x020A) {
            uint32_t r = send_msg(hwnd, msg, wp, lp);
            TM_INFO("DispatchMessage %04X wp %X lp %08X -> wndproc returned %X", msg, wp, lp, r);
            RET(r, 1);
        }
    }
    if (msg == WM_TIMER && lp) RET(tm_call_guest(lp, 4, hwnd, msg, wp, tm_guest_tick_ms()), 1);
    RET(send_msg(hwnd, msg, wp, lp), 1);
}
static void shim_PostQuitMessage(void) { g_quit_posted = 1; g_quit_code = ARG(0); RET(0, 1); }
static void shim_SetTimer(void) { RET(ARG(1) ? ARG(1) : 1, 4); }
static void shim_KillTimer(void) { RET(1, 2); }
static void shim_TrackMouseEvent(void) { RET(1, 1); }

/* ------------------------------------------------------------ input state */
static uint8_t g_keys[256];
void tm_user32_set_key(int vk, int down) { if (vk >= 0 && vk < 256) g_keys[vk] = down ? 0x80 : 0; }
static void shim_GetAsyncKeyState(void) {
    static int mlog = -1; static uint32_t seen;
    if (mlog < 0) { const char *e = getenv("TM_MOUSE_LOG"); mlog = e && *e && *e != '0'; }
    uint32_t vk = ARG(0) & 0xFF;
    if (mlog && vk < 32 && !(seen & (1u << vk))) { seen |= 1u << vk; TM_INFO("GetAsyncKeyState(%u) first used", vk); }
    RET(g_keys[vk] ? 0x8000 : 0, 1);
}
static void shim_GetKeyState(void) { RET(g_keys[ARG(0) & 0xFF] ? 0xFF80 : 0, 1); }
static int g_cursor_count;
static void shim_ShowCursor(void) {
    g_cursor_count += ARG(0) ? 1 : -1;
    static int mlog = -1;
    if (mlog < 0) { const char *e = getenv("TM_MOUSE_LOG"); mlog = e && *e && *e != '0'; }
    if (mlog) TM_INFO("ShowCursor(%u) -> %d", ARG(0), g_cursor_count);
    RET((uint32_t)g_cursor_count, 1);
}
void tm_input_mouse_warp(int x, int y);
static void shim_SetCursorPos(void) {
    static int mlog = -1; static uint32_t n;
    if (mlog < 0) { const char *e = getenv("TM_MOUSE_LOG"); mlog = e && *e && *e != '0'; }
    if (mlog && (n++ < 20 || (n & 255) == 0)) TM_INFO("SetCursorPos(%d, %d) [%u]", (int)ARG(0), (int)ARG(1), n);
    tm_input_mouse_warp((int)ARG(0), (int)ARG(1));
    RET(1, 2);
}
static void shim_LoadCursorW(void) { RET(0x7710 + (ARG(1) & 0xFF), 2); }
static void shim_LoadIconW(void) { RET(0x7720, 2); }
static void shim_LoadImageW(void) { RET(0x7730, 6); }
static void shim_GetIconInfo(void) { RET(0, 2); }

/* ------------------------------------------------------------ display/system */
static void shim_GetSystemMetrics(void) {
    switch (ARG(0)) {
    case 0: case 78: RET(g_screen_w, 1);   /* SM_CXSCREEN / CXVIRTUALSCREEN */
    case 1: case 79: RET(g_screen_h, 1);
    case 76: case 77: RET(0, 1);
    case 80: RET(1, 1);                    /* SM_CMONITORS */
    case 4: RET(23, 1);                    /* caption */
    case 32: case 33: RET(4, 1);           /* frame */
    default: RET(0, 1);
    }
}
static void shim_SystemParametersInfoW(void) {
    uint32_t action = ARG(0), pv = ARG(2);
    if (action == 0x30 && pv) { put_rect(pv, 0, 0, g_screen_w, g_screen_h); RET(1, 4); } /* SPI_GETWORKAREA */
    if (pv && action < 0x1000) { /* zero-fill unknown query outputs (BOOL/UINT) */ wr32(pv, 0); }
    RET(1, 4);
}
static void shim_MonitorFromWindow(void) { RET(0x7740, 2); }
static void shim_GetMonitorInfoW(void) {
    uint32_t p = ARG(1);
    put_rect(p + 4, 0, 0, g_screen_w, g_screen_h);
    put_rect(p + 20, 0, 0, g_screen_w, g_screen_h);
    wr32(p + 36, 1);
    if (rd32(p) >= 104) tm_u2w("\\\\.\\DISPLAY1", p + 40, 32);
    RET(1, 2);
}
static void shim_EnumDisplayDevicesW(void) {
    uint32_t idx = ARG(1), p = ARG(2);
    if (idx > 0 || ARG(0)) RET(0, 4);
    /* DISPLAY_DEVICEW: cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128] */
    tm_u2w("\\\\.\\DISPLAY1", p + 4, 32);
    tm_u2w("Android Vulkan GPU", p + 68, 128);
    wr32(p + 324, 0x5);   /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    tm_u2w("PCI\\VEN_5143&DEV_0830", p + 328, 128);
    RET(1, 4);
}
static void shim_GetProcessWindowStation(void) { RET(0x7750, 0); }
static void shim_GetUserObjectInformationW(void) {
    /* UOI_FLAGS: fInherit, fReserved, dwFlags (WSF_VISIBLE=1) */
    if (ARG(1) == 1 && ARG(3) >= 12) { wr32(ARG(2), 0); wr32(ARG(2) + 4, 0); wr32(ARG(2) + 8, 1); if (ARG(4)) wr32(ARG(4), 12); RET(1, 5); }
    RET(0, 5);
}
static void shim_GetDC(void) { RET(0x7760, 1); }
static void shim_ReleaseDC(void) { RET(1, 2); }
static void shim_MessageBoxW(void) {
    char t[1024], c[256];
    tm_w2u(ARG(1), t, sizeof t); tm_w2u(ARG(2), c, sizeof c);
    TM_WARN("MessageBoxW [%s] %s", c, t);
    tm_status("MessageBox: %s - %s", c, t);
    RET(1, 4);   /* IDOK */
}
static void shim_MessageBoxA(void) {
    TM_WARN("MessageBoxA [%s] %s", ARGS(2) ? ARGS(2) : "", ARGS(1) ? ARGS(1) : "");
    tm_status("MessageBox: %s - %s", ARGS(2) ? ARGS(2) : "", ARGS(1) ? ARGS(1) : "");
    RET(1, 4);
}

/* clipboard: empty */
static void shim_OpenClipboard(void) { RET(1, 1); }
static void shim_CloseClipboard(void) { RET(1, 0); }
static void shim_EmptyClipboard(void) { RET(1, 0); }
static void shim_IsClipboardFormatAvailable(void) { RET(0, 1); }
static void shim_GetClipboardData(void) { RET(0, 1); }
static void shim_SetClipboardData(void) { RET(ARG(1), 2); }

static const tm_shim_def user32_shims[] = {
#define U(n) TM_SHIM("user32.dll", n)
    U(RegisterClassExW), U(UnregisterClassW), U(CreateWindowExW), U(DestroyWindow), U(DefWindowProcW), U(ShowWindow),
    U(UpdateWindow), U(SetWindowPos), U(GetClientRect), U(GetWindowRect), U(SetRect), U(GetDesktopWindow), U(IsIconic),
    U(GetWindowLongW), U(SetWindowLongW), U(SetWindowTextW), U(SetFocus), U(GetFocus), U(SetForegroundWindow),
    U(GetForegroundWindow), U(BringWindowToTop), U(LockSetForegroundWindow), U(FindWindowExW), U(FlashWindowEx),
    U(SendMessageW), U(PeekMessageW), U(TranslateMessage), U(TranslateAcceleratorW), U(LoadAcceleratorsW),
    U(DispatchMessageW), U(PostQuitMessage), U(SetTimer), U(KillTimer), U(TrackMouseEvent), U(GetAsyncKeyState),
    U(GetKeyState), U(ShowCursor), U(SetCursorPos), U(LoadCursorW), U(LoadIconW), U(LoadImageW), U(GetIconInfo),
    U(GetSystemMetrics), U(SystemParametersInfoW), U(MonitorFromWindow), U(GetMonitorInfoW), U(EnumDisplayDevicesW),
    U(GetProcessWindowStation), U(GetUserObjectInformationW), U(GetDC), U(ReleaseDC), U(MessageBoxW), U(MessageBoxA),
    U(OpenClipboard), U(CloseClipboard), U(EmptyClipboard), U(IsClipboardFormatAvailable), U(GetClipboardData),
    U(SetClipboardData),
#undef U
};
void tm_user32_register(void) { tm_register_shims(user32_shims, sizeof user32_shims / sizeof user32_shims[0]); }
