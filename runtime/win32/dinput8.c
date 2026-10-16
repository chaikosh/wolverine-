/* DirectInput 8: keyboard, mouse and one virtual gamepad fed by SDL3 (and later the touch overlay). */
#include "../com.h"
#include "../platform.h"
#include "../haptics.h"
#include <stdlib.h>

static uint32_t vt_di, vt_dev, vt_eff, g_di_obj;

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_PAD = 3 };
typedef struct { int kind; uint32_t datasize; int32_t rmin, rmax, dead; } DIDev;

static const uint8_t GUID_SysKeyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t GUID_SysMouse[16]    = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t GUID_Pad[16]         = {0x57, 0x4F, 0x4C, 0x56, 0x50, 0x41, 0x44, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
/* axis GUIDs: A36D02E0-C9F3-11CF-BFC7-444553540000 family */
static void axis_guid(uint8_t *g, uint8_t lo) {
    const uint8_t base[16] = {0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    memcpy(g, base, 16); g[0] = lo;
}

#define M(n) static void n(CPU *c)
M(QI) { s32(ARG(c, 2), 0); RET(c, E_NOINTERFACE, 3); }
M(AddRef) { RET(c, com_addref(THIS), 1); }
M(Release) { RET(c, com_release(THIS), 1); }

/* ---------------------------------------------------------------- IDirectInput8A */
static uint32_t new_device(int kind) {
    DIDev *d = calloc(1, sizeof *d);
    d->kind = kind; d->rmin = 0; d->rmax = 65535;
    return com_new(vt_dev, d);
}
M(DI_CreateDevice) {
    const uint8_t *g = gptr(ARG(c, 1));
    int kind = !memcmp(g, GUID_SysKeyboard, 16) ? DEV_KEYBOARD : !memcmp(g, GUID_SysMouse, 16) ? DEV_MOUSE :
               !memcmp(g, GUID_Pad, 16) ? DEV_PAD : 0;
    if (!kind) { LOG("[dinput] CreateDevice: unknown GUID\n"); s32(ARG(c, 2), 0); RET(c, 0x80040154u, 4); return; }
    s32(ARG(c, 2), new_device(kind));
    LOG("[dinput] CreateDevice %s\n", kind == DEV_KEYBOARD ? "keyboard" : kind == DEV_MOUSE ? "mouse" : "gamepad");
    RET(c, HR_OK, 4);
}
static void fill_instance(uint32_t p, const uint8_t *guid, uint32_t type, const char *name) {
    memset(gptr(p), 0, 580);
    s32(p, 580);
    memcpy(gptr(p + 4), guid, 16); memcpy(gptr(p + 20), guid, 16);
    s32(p + 36, type);
    snprintf((char *)gptr(p + 40), 260, "%s", name);
    snprintf((char *)gptr(p + 300), 260, "%s", name);
}
M(DI_EnumDevices) {
    uint32_t type = ARG(c, 1), cb = ARG(c, 2), ref = ARG(c, 3);
    uint32_t inst = galloc(580, 1);
    uint32_t esp = c->esp;
    struct { int cls; const uint8_t *g; uint32_t t; const char *n; } devs[] = {
        {3, GUID_SysKeyboard, 0x0413, "Keyboard"}, {2, GUID_SysMouse, 0x0112, "Mouse"}, {4, GUID_Pad, 0x0215, "Wolverine Gamepad"}};
    for (int i = 0; i < 3; i++) {
        if (type && type != (uint32_t)devs[i].cls && !(type == 1 && devs[i].cls != 4)) continue;
        fill_instance(inst, devs[i].g, devs[i].t, devs[i].n);
        uint32_t r = rt_call_guest(c, cb, 2, inst, ref);
        if (!r) break;
    }
    c->esp = esp;
    gfree(inst);
    RET(c, HR_OK, 5);
}
M(DI_GetDeviceStatus) { RET(c, HR_OK, 2); }
M(DI_Initialize) { RET(c, HR_OK, 3); }

/* ---------------------------------------------------------------- IDirectInputDevice8A */
#define DEV ((DIDev *)com_host(THIS))
M(D_GetCapabilities) {
    DIDev *d = DEV; uint32_t p = ARG(c, 1);
    s32(p + 4, 1);   /* DIDC_ATTACHED */
    s32(p + 8, d->kind == DEV_KEYBOARD ? 0x0413 : d->kind == DEV_MOUSE ? 0x0112 : 0x0215);
    s32(p + 12, d->kind == DEV_PAD ? 6 : d->kind == DEV_MOUSE ? 3 : 0);
    s32(p + 16, d->kind == DEV_PAD ? 16 : d->kind == DEV_MOUSE ? 4 : 128);
    s32(p + 20, d->kind == DEV_PAD ? 1 : 0);
    RET(c, HR_OK, 2);
}
M(D_EnumObjects) {
    DIDev *d = DEV;
    uint32_t cb = ARG(c, 1), ref = ARG(c, 2), flags = ARG(c, 3);
    if (d->kind != DEV_PAD) { RET(c, HR_OK, 4); return; }
    uint32_t o = galloc(316, 1), esp = c->esp;
    static const uint8_t axes[6] = {0xE0, 0xE1, 0xE2, 0xF4, 0xF5, 0xE3};   /* X Y Z Rx Ry Rz */
    static const char *const names[6] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation"};
    if (flags == 0 || (flags & 3)) {   /* DIDFT_ALL / DIDFT_AXIS */
        for (int i = 0; i < 6; i++) {
            memset(gptr(o), 0, 316); s32(o, 316);
            axis_guid(gptr(o + 4), axes[i]);
            s32(o + 20, 4u * (uint32_t)i); s32(o + 24, 2u | ((uint32_t)i << 8)); s32(o + 28, 0);
            snprintf((char *)gptr(o + 32), 260, "%s", names[i]);
            if (!rt_call_guest(c, cb, 2, o, ref)) break;
        }
    }
    c->esp = esp;
    gfree(o);
    RET(c, HR_OK, 4);
}
M(D_GetProperty) {
    DIDev *d = DEV; uint32_t prop = ARG(c, 1), p = ARG(c, 2);
    if (prop == 4) { s32(p + 16, (uint32_t)d->rmin); s32(p + 20, (uint32_t)d->rmax); }
    else if (prop == 5) s32(p + 16, (uint32_t)d->dead);
    else if (prop == 1) s32(p + 16, 64);
    RET(c, HR_OK, 3);
}
M(D_SetProperty) {
    DIDev *d = DEV; uint32_t prop = ARG(c, 1), p = ARG(c, 2);
    if (prop == 4) { d->rmin = (int32_t)g32(p + 16); d->rmax = (int32_t)g32(p + 20); }
    else if (prop == 5) d->dead = (int32_t)g32(p + 16);
    RET(c, HR_OK, 3);
}
M(D_Acquire) { RET(c, HR_OK, 1); }
M(D_Unacquire) { RET(c, HR_OK, 1); }
static int32_t scale_axis(DIDev *d, int16_t v) {
    double f = (v + 32768.0) / 65535.0;
    double dz = d->dead / 10000.0;
    double cen = f * 2 - 1;
    if (cen > -dz && cen < dz) f = 0.5;
    return (int32_t)(d->rmin + f * (d->rmax - d->rmin) + 0.5);
}
static uint32_t pov_of(void) {
    int u = g_pad.buttons[10] != 0, dn = g_pad.buttons[11] != 0, l = g_pad.buttons[12] != 0, r = g_pad.buttons[13] != 0;
    if (u && r) return 4500; if (r && dn) return 13500; if (dn && l) return 22500; if (l && u) return 31500;
    if (u) return 0; if (r) return 9000; if (dn) return 18000; if (l) return 27000;
    return 0xFFFFFFFFu;
}
M(D_GetDeviceState) {
    DIDev *d = DEV; uint32_t n = ARG(c, 1), p = ARG(c, 2);
    plat_pump();
    memset(gptr(p), 0, n);
    if (d->kind == DEV_KEYBOARD) memcpy(gptr(p), g_dik_state, n < 256 ? n : 256);
    else if (d->kind == DEV_MOUSE) {
        s32(p, (uint32_t)g_mouse_dx); s32(p + 4, (uint32_t)g_mouse_dy); s32(p + 8, (uint32_t)g_mouse_wheel);
        g_mouse_dx = g_mouse_dy = g_mouse_wheel = 0;
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) gptr(p + 12)[i] = g_mouse_buttons[i];
    } else {
        /* DIJOYSTATE / DIJOYSTATE2 layout */
        s32(p + 0, (uint32_t)scale_axis(d, g_pad.lx));
        s32(p + 4, (uint32_t)scale_axis(d, g_pad.ly));
        /* Wolvie.INI Type=5 reads camera from Z (CAMLR=1) and Rz (CAMUD=2) */
        s32(p + 8, (uint32_t)scale_axis(d, g_pad.rx));
        s32(p + 12, (uint32_t)scale_axis(d, (int16_t)(g_pad.lt * 128 - 32768)));
        s32(p + 16, (uint32_t)scale_axis(d, (int16_t)(g_pad.rt * 128 - 32768)));
        s32(p + 20, (uint32_t)scale_axis(d, g_pad.ry));
        s32(p + 32, pov_of()); s32(p + 36, 0xFFFFFFFFu); s32(p + 40, 0xFFFFFFFFu); s32(p + 44, 0xFFFFFFFFu);
        for (int i = 0; i < 10 && 48 + i < (int)n; i++) gptr(p + 48)[i] = g_pad.buttons[i];
        if (n > 58) { gptr(p + 48)[10] = g_pad.buttons[10]; gptr(p + 48)[11] = g_pad.buttons[11]; }
    }
    RET(c, HR_OK, 3);
}
M(D_GetDeviceData) {
    DIDev *d = DEV;
    uint32_t sz = ARG(c, 1), buf = ARG(c, 2), pn = ARG(c, 3), flags = ARG(c, 4);
    uint32_t cap = g32(pn), n = 0;
    plat_pump();
    if (d->kind == DEV_KEYBOARD && !(flags & 1)) {
        KeyEvt e;
        static uint32_t seq;
        while (n < cap && buf && plat_pop_key(&e)) {
            uint32_t o = buf + n * sz;
            memset(gptr(o), 0, sz);
            s32(o, e.dik); s32(o + 4, e.down ? 0x80 : 0); s32(o + 8, e.time); s32(o + 12, ++seq);
            n++;
        }
    }
    s32(pn, n);
    RET(c, HR_OK, 5);
}
M(D_SetDataFormat) { DEV->datasize = g32(ARG(c, 1) + 12); RET(c, HR_OK, 2); }
M(D_SetEventNotification) { RET(c, HR_OK, 2); }
M(D_SetCooperativeLevel) { RET(c, HR_OK, 3); }
M(D_GetDeviceInfo) {
    DIDev *d = DEV;
    fill_instance(ARG(c, 1), d->kind == DEV_KEYBOARD ? GUID_SysKeyboard : d->kind == DEV_MOUSE ? GUID_SysMouse : GUID_Pad,
                  d->kind == DEV_KEYBOARD ? 0x0413 : d->kind == DEV_MOUSE ? 0x0112 : 0x0215,
                  d->kind == DEV_PAD ? "Wolverine Gamepad" : "Device");
    RET(c, HR_OK, 2);
}
M(D_Poll) { plat_pump(); RET(c, HR_OK, 1); }
M(D_EnumEffects) { RET(c, HR_OK, 4); }
M(D_GetObjectInfo) { RET(c, 0x80070002u, 4); }
static void log_effect(const char *what, uint32_t eff) {
    if (!eff) { LOG("[ff] %s\n", what); return; }
    uint32_t tsp = g32(eff + 44), tsn = g32(eff + 40);
    LOG("[ff] %s flags=%X duration=%u gain=%u axes=%u typeparams=%u first=%d\n", what, g32(eff + 4), g32(eff + 8),
        g32(eff + 16), g32(eff + 28), tsn, tsp && tsn ? (int32_t)g32(tsp) : 0);
}
typedef struct { uint32_t duration_us, gain; } Eff;   /* DIEFFECT: dwDuration (+8), dwGain (+16) */
static void eff_update(Eff *e, uint32_t p, uint32_t flags) {
    if (!p) return;
    if (flags & 0x1) e->duration_us = g32(p + 8);   /* DIEP_DURATION */
    if (flags & 0x4) e->gain = g32(p + 16);         /* DIEP_GAIN */
}
M(D_CreateEffect) {
    Eff *e = calloc(1, sizeof *e);
    e->duration_us = 300000; e->gain = 10000;
    eff_update(e, ARG(c, 2), 0x5);
    s32(ARG(c, 3), com_new(vt_eff, e));
    RET(c, HR_OK, 5);
}
M(E_Initialize) { RET(c, HR_OK, 4); }
M(E_GetEffectGuid) { memset(gptr(ARG(c, 1)), 0, 16); RET(c, HR_OK, 2); }
M(E_GetParameters) { RET(c, HR_OK, 3); }
M(E_SetParameters) { Eff *e = com_host(THIS); if (e) eff_update(e, ARG(c, 1), ARG(c, 2)); RET(c, HR_OK, 3); }
M(E_Start) {
    Eff *e = com_host(THIS);
    uint32_t it = ARG(c, 1);
    float sec = (!e || e->duration_us == 0xFFFFFFFFu || it == 0xFFFFFFFFu) ? 0.f : e->duration_us / 1e6f * (it ? (float)it : 1.f);
    haptics_play(e ? e->gain / 10000.f : 1.f, sec);
    RET(c, HR_OK, 3);
}
M(E_Stop) { haptics_stop(); RET(c, HR_OK, 1); }
M(E_GetEffectStatus) { s32(ARG(c, 1), 0); RET(c, HR_OK, 2); }
M(E_Download) { RET(c, HR_OK, 1); }
M(E_Unload) { RET(c, HR_OK, 1); }
M(E_Escape) { RET(c, HR_OK, 2); }
M(D_SendForceFeedbackCommand) { RET(c, HR_OK, 2); }
M(D_Escape) { RET(c, HR_OK, 2); }

static void build(void) {
    static const char *const di_n[] = {"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices", "GetDeviceStatus",
                                       "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices"};
    static const GuestFn di_f[] = {QI, AddRef, Release, DI_CreateDevice, DI_EnumDevices, DI_GetDeviceStatus, NULL, DI_Initialize, NULL, NULL, NULL};
    vt_di = com_vtable("IDirectInput8A", di_n, di_f, 11);
    static const char *const dv_n[] = {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects", "GetProperty",
        "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification",
        "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects",
        "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll",
        "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap", "SetActionMap", "GetImageInfo"};
    static const GuestFn dv_f[] = {QI, AddRef, Release, D_GetCapabilities, D_EnumObjects, D_GetProperty, D_SetProperty, D_Acquire,
        D_Unacquire, D_GetDeviceState, D_GetDeviceData, D_SetDataFormat, D_SetEventNotification, D_SetCooperativeLevel,
        D_GetObjectInfo, D_GetDeviceInfo, NULL, NULL, D_CreateEffect, D_EnumEffects, NULL, NULL, D_SendForceFeedbackCommand,
        NULL, D_Escape, D_Poll, NULL, NULL, NULL, NULL, NULL, NULL};
    vt_dev = com_vtable("IDirectInputDevice8A", dv_n, dv_f, 32);
    static const char *const ef_n[] = {"QueryInterface", "AddRef", "Release", "Initialize", "GetEffectGuid", "GetParameters",
        "SetParameters", "Start", "Stop", "GetEffectStatus", "Download", "Unload", "Escape"};
    static const GuestFn ef_f[] = {QI, AddRef, Release, E_Initialize, E_GetEffectGuid, E_GetParameters, E_SetParameters,
        E_Start, E_Stop, E_GetEffectStatus, E_Download, E_Unload, E_Escape};
    vt_eff = com_vtable("IDirectInputEffect", ef_n, ef_f, 13);
}

void shim_DINPUT8_DirectInput8Create(CPU *c) {
    if (!vt_di) build();
    if (!g_di_obj) g_di_obj = com_new(vt_di, NULL);
    s32(ARG(c, 3), g_di_obj);
    LOG("[dinput] DirectInput8Create(version %X)\n", ARG(c, 1));
    RET(c, HR_OK, 5);
}
