/* USER32 / GDI32 / ADVAPI32 shims on top of SDL3. */
#include "win32.h"
#include "../platform.h"
#include <stdlib.h>
#include <strings.h>

#define S(name) void shim_USER32_##name(CPU *c)
#define HWND_MAIN 0x00010010u

static struct { char name[64]; uint32_t proc; } g_classes[16];
static int g_nclasses;
static uint32_t g_wndproc;
static int g_cursor_count;

S(RegisterClassA) {
    uint32_t wc = ARG(c, 0);
    if (g_nclasses < 16) {
        snprintf(g_classes[g_nclasses].name, 64, "%s", gstr(g32(wc + 36)));
        g_classes[g_nclasses++].proc = g32(wc + 4);
    }
    LOG("[user32] RegisterClass %s wndproc=%08X\n", gstr(g32(wc + 36)), g32(wc + 4));
    RET(c, 0xC000u + (uint32_t)g_nclasses, 1);
}
S(UnregisterClassA) { RET(c, 1, 2); }
S(CreateWindowExA) {
    const char *cls = gstr(ARG(c, 1)), *title = gstr(ARG(c, 2));
    int w = (int)ARG(c, 6), h = (int)ARG(c, 7);
    for (int i = 0; i < g_nclasses; i++) if (cls && !strcasecmp(cls, g_classes[i].name)) g_wndproc = g_classes[i].proc;
    if (w <= 0 || w > 8192) w = 1366;
    if (h <= 0 || h > 8192) h = 768;
    LOG("[user32] CreateWindow \"%s\" %dx%d style=%08X\n", title ? title : "", w, h, ARG(c, 3));
    plat_create_window(title ? title : "Wolverine", w, h);
    plat_push_msg(0x001C, 1, 0);   /* WM_ACTIVATEAPP */
    plat_push_msg(0x0006, 1, 0);   /* WM_ACTIVATE */
    RET(c, HWND_MAIN, 12);
}
S(ShowWindow) { RET(c, 1, 2); }
S(UpdateWindow) { RET(c, 1, 1); }
S(DestroyWindow) { RET(c, 1, 1); }
S(MoveWindow) { RET(c, 1, 6); }
S(SetFocus) { RET(c, HWND_MAIN, 1); }
S(GetFocus) { RET(c, HWND_MAIN, 0); }
S(SetActiveWindow) { RET(c, HWND_MAIN, 1); }
S(SetForegroundWindow) { RET(c, 1, 1); }
S(GetWindowLongA) { RET(c, 0, 2); }
S(AdjustWindowRect) { RET(c, 1, 3); }
S(LoadIconA) { RET(c, 0x00020001u, 2); }
S(LoadCursorA) { RET(c, 0x00020002u, 2); }
S(SetCursor) { RET(c, 0, 1); }
S(ShowCursor) { g_cursor_count += ARG(c, 0) ? 1 : -1; RET(c, (uint32_t)g_cursor_count, 1); }
S(GetSystemMetrics) {
    uint32_t i = ARG(c, 0);
    RET(c, i == 0 ? 1366u : i == 1 ? 768u : 0u, 1);
}
S(MessageBoxA) {
    LOG("[user32] MessageBox \"%s\": %s\n", gstr(ARG(c, 2)) ? gstr(ARG(c, 2)) : "", gstr(ARG(c, 1)) ? gstr(ARG(c, 1)) : "");
    RET(c, 1, 4);
}
S(PeekMessageA) {
    uint32_t msg = ARG(c, 0), remove = ARG(c, 4);
    static WinMsg pending;
    static int have;
    if (!have) {
        plat_pump();
        have = plat_pop_msg(&pending);
    }
    if (!have) { RET(c, 0, 5); return; }
    s32(msg, HWND_MAIN); s32(msg + 4, pending.msg); s32(msg + 8, pending.wparam); s32(msg + 12, pending.lparam);
    s32(msg + 16, 0); s32(msg + 20, 0); s32(msg + 24, 0);
    if (remove & 1) have = 0;
    RET(c, 1, 5);
}
S(TranslateMessage) { RET(c, 0, 1); }
S(DispatchMessageA) {
    uint32_t m = ARG(c, 0);
    uint32_t r = 0;
    if (g_wndproc) {
        uint32_t esp = c->esp;
        r = rt_call_guest(c, g_wndproc, 4, g32(m), g32(m + 4), g32(m + 8), g32(m + 12));
        c->esp = esp;
    }
    RET(c, r, 1);
}
S(DefWindowProcA) {
    uint32_t msg = ARG(c, 1);
    if (msg == 0x0010) { LOG("[user32] WM_CLOSE -> exit\n"); exit(0); }
    RET(c, 0, 4);
}

/* GDI32 */
void shim_GDI32_GetStockObject(CPU *c) { RET(c, 0x00030000u + ARG(c, 0), 1); }
void shim_GDI32_DeleteObject(CPU *c) { RET(c, 1, 1); }

/* ADVAPI32: no registry; callers fall back to defaults */
void shim_ADVAPI32_RegOpenKeyExA(CPU *c) { LOG("[reg] open %s\n", gstr(ARG(c, 1)) ? gstr(ARG(c, 1)) : ""); RET(c, 2, 5); }
void shim_ADVAPI32_RegOpenKeyA(CPU *c) { LOG("[reg] open %s\n", gstr(ARG(c, 1)) ? gstr(ARG(c, 1)) : ""); RET(c, 2, 3); }
void shim_ADVAPI32_RegQueryValueExA(CPU *c) { RET(c, 2, 6); }
void shim_ADVAPI32_RegCloseKey(CPU *c) { RET(c, 0, 1); }
