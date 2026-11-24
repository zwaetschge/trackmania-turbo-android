/*
 * dinput8.c - DirectInput8 over the guest COM bridge.
 *
 * Enough of IDirectInput8 / IDirectInputDevice8 for the engine's input system.
 * The keyboard reports the platform key state (input.c), immediate and
 * buffered; the mouse reports the touch screen / mouse (input.c); no DirectInput joystick is
 * enumerated - the gamepad is exposed through XInput (controller 0).
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include "tmcom.h"
#include <stdlib.h>
#include <string.h>

#define S_OK 0u
#define DI_OK 0u
#define E_NOTIMPL 0x80004001u
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_NOTACQUIRED 0x8007001Cu

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOYSTICK = 3 };

static tm_com_class c_di, c_dev;
typedef struct { int kind; int acquired; uint32_t fmt_size; } DevState;

/* ---- IDirectInputDevice8 ---- */
static void dev_qi(void) { if (ARG(2)) wr32(ARG(2), ARG(0)); RET(S_OK, 3); }
static void dev_addref(void) { RET(tm_com_addref(ARG(0)), 1); }
static void dev_release(void) {
    RET(tm_com_release(ARG(0), free), 1);
}
static void dev_GetCapabilities(void) {
    uint32_t p = ARG(1); DevState *d = tm_com_host(ARG(0));
    if (p && d) {
        /* DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, ... */
        wr32(p + 4, 0x00000001);                      /* DIDC_ATTACHED */
        wr32(p + 8, d->kind);
        wr32(p + 12, d->kind == DEV_MOUSE ? 3 : 0);   /* axes */
        wr32(p + 16, d->kind == DEV_KEYBOARD ? 256 : d->kind == DEV_MOUSE ? 8 : 0);
        wr32(p + 20, 0);
    }
    RET(DI_OK, 2);
}
static void dev_EnumObjects(void) { RET(DI_OK, 4); }
static void dev_GetProperty(void) { RET(S_OK, 3); }
static void dev_SetProperty(void) { RET(DI_OK, 3); }
static void dev_Acquire(void) { DevState *d = tm_com_host(ARG(0)); if (d) d->acquired = 1; RET(DI_OK, 1); }
static void dev_Unacquire(void) { DevState *d = tm_com_host(ARG(0)); if (d) d->acquired = 0; RET(DI_OK, 1); }
static uint32_t g_reads[4][2];      /* per device kind: GetDeviceState, GetDeviceData (TM_MOUSE_LOG) */
static void read_note(int kind, int which) {
    static int on = -1; static uint64_t t0;
    if (on < 0) { const char *e = getenv("TM_MOUSE_LOG"); on = e && *e && *e != '0'; }
    if (!on || kind < 0 || kind > 3) return;
    g_reads[kind][which]++;
    uint64_t now = tm_now_ns();
    if (!t0) t0 = now;
    if (now - t0 > 5000000000ull) {
        TM_INFO("DirectInput8 reads in 5 s: keyboard state %u data %u, mouse state %u data %u", g_reads[1][0], g_reads[1][1], g_reads[2][0], g_reads[2][1]);
        memset(g_reads, 0, sizeof g_reads); t0 = now;
    }
}
static void dev_GetDeviceState(void) {
    uint32_t cb = ARG(1), buf = ARG(2);
    DevState *d = tm_com_host(ARG(0));
    if (d) read_note(d->kind, 0);
    if (!d || !d->acquired) RET(DIERR_NOTACQUIRED, 3);
    if (buf && cb) {
        memset(G2H(buf), 0, cb);
        if (d->kind == DEV_KEYBOARD) tm_input_kbd_state(G2H(buf), cb);
        else if (d->kind == DEV_MOUSE) tm_input_mouse_state(G2H(buf), cb);
    }
    RET(DI_OK, 3);
}
static void dev_GetDeviceData(void) {
    /* (this, cbObjectData, rgdod, pdwInOut, dwFlags): 5 stack slots.
     * DIDEVICEOBJECTDATA = { dwOfs, dwData, dwTimeStamp, dwSequence[, uAppData] }.
     * rgdod == NULL only counts (and, without DIGDD_PEEK, flushes). */
    uint32_t cb = ARG(1), out = ARG(2), pcount = ARG(3), flags = ARG(4);
    DevState *d = tm_com_host(ARG(0));
    if (d) read_note(d->kind, 1);
    uint32_t want = pcount ? rd32(pcount) : 0, got = 0;
    if (d && d->kind == DEV_KEYBOARD && pcount) {
        enum { MAXEV = 64 };
        uint32_t dik[MAXEV], data[MAXEV], tim[MAXEV], seq[MAXEV];
        int max = want > MAXEV ? MAXEV : (int)want;
        if (!out) got = (uint32_t)tm_input_kbd_events(NULL, NULL, NULL, NULL, max, !(flags & 1));
        else {
            got = (uint32_t)tm_input_kbd_events(dik, data, tim, seq, max, !(flags & 1));
            for (uint32_t i = 0; i < got && cb >= 16; i++) {
                uint32_t e = out + i * cb;
                wr32(e, dik[i]); wr32(e + 4, data[i]); wr32(e + 8, tim[i]); wr32(e + 12, seq[i]);
                if (cb >= 20) wr32(e + 16, 0);
            }
        }
    }
    if (d && d->kind == DEV_MOUSE && pcount) {
        enum { MAXEV = 64 };
        uint32_t ofs[MAXEV], data[MAXEV], tim[MAXEV], seq[MAXEV];
        int max = want > MAXEV ? MAXEV : (int)want;
        if (!out) got = (uint32_t)tm_input_mouse_events(NULL, NULL, NULL, NULL, max, !(flags & 1));
        else {
            got = (uint32_t)tm_input_mouse_events(ofs, data, tim, seq, max, !(flags & 1));
            static int mlog = -1;
            if (mlog < 0) { const char *e = getenv("TM_MOUSE_LOG"); mlog = e && *e && *e != '0'; }
            for (uint32_t i = 0; mlog && i < got; i++)
                if (ofs[i] >= 12) TM_INFO("DirectInput8 mouse event to the game: ofs %u data %X time %u seq %u (cb %u, want %u, flags %X)", ofs[i], data[i], tim[i], seq[i], cb, want, flags);
            for (uint32_t i = 0; i < got && cb >= 16; i++) {
                uint32_t e = out + i * cb;
                wr32(e, ofs[i]); wr32(e + 4, data[i]); wr32(e + 8, tim[i]); wr32(e + 12, seq[i]);
                if (cb >= 20) wr32(e + 16, 0);
            }
        }
    }
    if (pcount) wr32(pcount, got);
    RET(DI_OK, 5);
}
static void dev_SetDataFormat(void) {
    DevState *d = tm_com_host(ARG(0));
    uint32_t f = ARG(1);
    if (d && f) {
        d->fmt_size = rd32(f + 4);
        /* DIDATAFORMAT { dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf }
         * DIOBJECTDATAFORMAT { pguid, dwOfs, dwType, dwFlags } */
        uint32_t datasize = rd32(f + 12), n = rd32(f + 16), objs = rd32(f + 20), osz = rd32(f + 4);
        TM_INFO("DirectInput8 SetDataFormat kind %d: flags %X, data %u bytes, %u objects", d->kind, rd32(f + 8), datasize, n);
        if (d->kind == DEV_MOUSE)
            for (uint32_t i = 0; i < n && i < 12 && objs; i++) {
                uint32_t o = objs + i * (osz ? osz : 16), g = rd32(o);
                TM_INFO("  object %u: guid %08X ofs %u type %08X flags %X", i, g ? rd32(g) : 0, rd32(o + 4), rd32(o + 8), rd32(o + 12));
            }
    }
    RET(DI_OK, 2);
}
static void dev_SetEventNotification(void) { RET(DI_OK, 2); }
static void dev_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dev_GetObjectInfo(void) { RET(E_NOTIMPL, 4); }
static void dev_GetDeviceInfo(void) { RET(DI_OK, 2); }
static void dev_Poll(void) { DevState *d = tm_com_host(ARG(0)); RET(d && d->acquired ? DI_OK : DIERR_NOTACQUIRED, 1); }
/* Generic returns. The count is the method's stack slots INCLUDING this
 * (stdcall): a wrong count shifts the guest stack by 4 bytes per missing slot. */
static void dev_ret_ok2(void) { RET(DI_OK, 2); }
static void dev_ret_ok3(void) { RET(DI_OK, 3); }
static void dev_ret_ok4(void) { RET(DI_OK, 4); }
static void dev_ret_ok5(void) { RET(DI_OK, 5); }
static void dev_ret_ni3(void) { RET(E_NOTIMPL, 3); }
static void dev_ret_ni5(void) { RET(E_NOTIMPL, 5); }
static tm_shim_fn dev_methods[] = {
    dev_qi, dev_addref, dev_release,
    dev_GetCapabilities, dev_EnumObjects, dev_GetProperty, dev_SetProperty, dev_Acquire, dev_Unacquire,
    dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat, dev_SetEventNotification, dev_SetCooperativeLevel,
    dev_GetObjectInfo, dev_GetDeviceInfo,
    dev_ret_ok3 /*RunControlPanel(this,hwnd,flags)*/,
    dev_ret_ok4 /*Initialize(this,hinst,version,rguid)*/,
    dev_ret_ni5 /*CreateEffect(this,rguid,lpeff,ppdeff,punkOuter)*/,
    dev_ret_ok4 /*EnumEffects(this,cb,ref,type)*/,
    dev_ret_ni3 /*GetEffectInfo(this,pdei,rguid)*/,
    dev_ret_ok2 /*GetForceFeedbackState(this,out)*/,
    dev_ret_ok2 /*SendForceFeedbackCommand(this,flags)*/,
    dev_ret_ok4 /*EnumCreatedEffectObjects(this,cb,ref,fl)*/,
    dev_ret_ok2 /*Escape(this,pesc)*/,
    dev_Poll,
    dev_ret_ok5 /*SendDeviceData(this,cb,rgdod,pdwInOut,fl)*/,
    dev_ret_ok5 /*EnumEffectsInFile(this,file,cb,ref,flags)*/,
    dev_ret_ok5 /*WriteEffectToFile(this,file,entries,rgeft,flags)*/,
    dev_ret_ok4 /*BuildActionMap(this,lpdiaf,user,flags)*/,
    dev_ret_ok4 /*SetActionMap(this,lpdiaf,user,flags)*/,
    dev_ret_ok2 /*GetImageInfo(this,hdr)*/ };

/* ---- IDirectInput8 ---- */
static void di_qi(void) { if (ARG(2)) wr32(ARG(2), ARG(0)); RET(S_OK, 3); }
static void di_addref(void) { RET(tm_com_addref(ARG(0)), 1); }
static void di_release(void) { RET(tm_com_release(ARG(0), free), 1); }
static void di_CreateDevice(void) {
    /* (this, REFGUID rguid, LPDIRECTINPUTDEVICE8* out, punkOuter). The GUID's
     * first dword distinguishes the system keyboard/mouse (GUID_SysKeyboard /
     * GUID_SysMouse); anything else is treated as a joystick. */
    uint32_t guid = ARG(1), out = ARG(2);
    int kind = DEV_JOYSTICK;
    if (guid) {
        uint32_t d1 = rd32(guid);
        if (d1 == 0x6F1D2B61u) kind = DEV_KEYBOARD;   /* GUID_SysKeyboard */
        else if (d1 == 0x6F1D2B60u) kind = DEV_MOUSE; /* GUID_SysMouse */
    }
    DevState *d = calloc(1, sizeof *d); d->kind = kind;
    uint32_t obj = tm_com_new(&c_dev, d);
    if (out) wr32(out, obj);
    TM_INFO("DirectInput8 CreateDevice kind %d -> %08X", kind, obj);
    RET(DI_OK, 4);
}
static void di_EnumDevices(void) {
    /* No devices enumerated through here (keyboard/mouse are created directly);
     * no joysticks yet, so the callback is never invoked. */
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) { RET(S_OK, 2); }
static void di_RunControlPanel(void) { RET(DI_OK, 3); }
static void di_Initialize(void) { RET(DI_OK, 3); }
static void di_FindDevice(void) { RET(S_OK, 4); }
static void di_ret_ok5(void) { RET(DI_OK, 5); }
static void di_ret_ok6(void) { RET(DI_OK, 6); }
static tm_shim_fn di_methods[] = {
    di_qi, di_addref, di_release,
    di_CreateDevice, di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel, di_Initialize, di_FindDevice,
    di_ret_ok6 /*EnumDevicesBySemantics*/, di_ret_ok5 /*ConfigureDevices(this,cb,params,flags,ref)*/ };

static void shim_DirectInput8Create(void) {
    /* (hinst, version, riid, ppvOut, punkOuter) */
    uint32_t out = ARG(3);
    uint32_t obj = tm_com_new(&c_di, NULL);
    if (out) wr32(out, obj);
    TM_INFO("DirectInput8Create -> %08X", obj);
    RET(S_OK, 5);
}

/* XInput: pads 0..3 come from the platform layer (input.c); a slot without a controller
 * answers ERROR_DEVICE_NOT_CONNECTED (1167). */
static void shim_XInputGetState(void) {
    /* (dwUserIndex, XINPUT_STATE*) */
    if (ARG(1)) memset(G2H(ARG(1)), 0, 16);
    if (ARG(0) < 4 && ARG(1) && tm_input_pad_get_slot((int)ARG(0), G2H(ARG(1)))) RET(0, 2);
    RET(1167, 2);
}
static void shim_XInputSetState(void) {
    /* (dwUserIndex, XINPUT_VIBRATION* {WORD wLeftMotorSpeed, wRightMotorSpeed}) */
    uint8_t st[16];
    if (ARG(0) >= 4 || !tm_input_pad_get_slot((int)ARG(0), st)) RET(1167, 2);
    if (ARG(1)) tm_input_rumble((int)ARG(0), rd16(ARG(1)), rd16(ARG(1) + 2));
    RET(0, 2);
}

static const tm_shim_def di_shims[] = {
    TM_SHIM("dinput8.dll", DirectInput8Create),
    TM_SHIM("xinput9_1_0.dll", XInputGetState), TM_SHIM("xinput9_1_0.dll", XInputSetState),
};
void tm_dinput8_register(void) {
    c_di = (tm_com_class){ "IDirectInput8", (int)(sizeof di_methods / sizeof di_methods[0]), di_methods, NULL, 0x44493800, 0 };
    c_dev = (tm_com_class){ "IDirectInputDevice8", (int)(sizeof dev_methods / sizeof dev_methods[0]), dev_methods, NULL, 0x44494400, 0 };
    tm_register_shims(di_shims, sizeof di_shims / sizeof di_shims[0]);
    tm_input_init();
}
