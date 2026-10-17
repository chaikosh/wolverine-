/* Touch controls laid out like an Xbox pad (floating left stick, camera drag, A/B/X/Y, LB/RB/LT/RT, Start, RS),
   each button showing a pictogram of what it does. */
#include "touch.h"
#include "icons.h"
#include "platform.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>

/* Pad button numbers match SDL gamepads in platform.c: A0 B1 X2 Y3 LB4 RB5 Back6 Start7 LS8 RS9 LT10 RT11 */
enum { T_A, T_B, T_X, T_Y, T_LB, T_RB, T_LT, T_RT, T_BACK, T_START, T_RS, NBTN };
typedef struct { float x, y, r; int pad_button, toggle, icon; float col[3]; } Btn;   /* units of screen height */
static Btn g_btn[NBTN];
static int g_down[NBTN], g_latched[NBTN];

#define MAXT 10
static struct { int used; uint64_t id; int role; float x0, y0, x, y; } g_t[MAXT];   /* role: -1 stick, -2 camera, >=0 button */
static float g_w = 1, g_h = 1;
static int g_layout_done;

int touch_enabled(void) {
#if TARGET_OS_IPHONE
    return 1;
#else
    return getenv("TOUCH_UI") != NULL;   /* preview the overlay on the Mac */
#endif
}

static float g_inset_l, g_inset_r;

static void layout(void) {
    /* Thumb-reach layout (as in mobile shooters/action games): main attack is the biggest button near the
       right thumb's rest point, other actions on an arc around it; only pause sits at the top.
       Both sides respect the safe area so nothing lands under the Dynamic Island. */
    float a = g_w / g_h, il, ir;
    plat_safe_insets(&il, &ir);
    g_inset_l = fmaxf(il / g_h, 0.03f);
    g_inset_r = fmaxf(ir / g_h, 0.03f);
    float R = a - g_inset_r, L = g_inset_l;
    g_btn[T_X]     = (Btn){R - 0.22f, 0.76f, 0.105f, 2, 0, ICON_PUNCH, {0.35f, 0.60f, 1.00f}};   /* punch / slash */
    g_btn[T_Y]     = (Btn){R - 0.47f, 0.86f, 0.085f, 3, 0, ICON_KICK, {1.00f, 0.85f, 0.25f}};   /* kick */
    g_btn[T_A]     = (Btn){R - 0.45f, 0.60f, 0.085f, 0, 0, ICON_JUMP, {0.35f, 0.85f, 0.35f}};   /* jump / confirm */
    g_btn[T_B]     = (Btn){R - 0.22f, 0.48f, 0.085f, 1, 0, ICON_ACTION, {0.95f, 0.35f, 0.35f}};   /* action / strike */
    g_btn[T_RB]    = (Btn){R - 0.70f, 0.84f, 0.070f, 5, 0, ICON_CLAWS, {0.85f, 0.85f, 0.85f}};  /* claws (double tap: rage) */
    g_btn[T_LT]    = (Btn){R - 0.06f, 0.62f, 0.060f, 10, 1, ICON_CROUCH, {0.85f, 0.85f, 0.85f}}; /* crouch: toggle */
    g_btn[T_RT]    = (Btn){R - 0.66f, 0.60f, 0.060f, 11, 1, ICON_SENSES, {0.85f, 0.85f, 0.85f}}; /* senses: toggle */
    g_btn[T_RS]    = (Btn){R - 0.07f, 0.36f, 0.050f, 9, 0, ICON_CAMERA, {0.85f, 0.85f, 0.85f}};  /* camera reset */
    g_btn[T_LB]    = (Btn){L + 0.09f, 0.80f, 0.065f, 4, 1, ICON_STEALTH, {0.85f, 0.85f, 0.85f}};  /* stealth: toggle, left thumb */
    g_btn[T_START] = (Btn){L + 0.05f, 0.09f, 0.050f, 7, 0, ICON_PAUSE, {0.85f, 0.85f, 0.85f}};   /* pause */
    g_btn[T_BACK]  = (Btn){-1.0f, -1.0f, 0.0f, 6, 0, -1, {0.85f, 0.85f, 0.85f}};         /* hidden on touch */
    g_layout_done = 1;
}

void touch_event(int type, uint64_t id, float nx, float ny) {
    int w, h;
    plat_window_size(&w, &h);
    if (w > 0 && h > 0) { g_w = (float)w; g_h = (float)h; }
    if (!g_layout_done) layout();
    float x = nx * g_w / g_h, y = ny;
    if (type == 0) {
        int slot = -1;
        for (int i = 0; i < MAXT; i++) if (!g_t[i].used) { slot = i; break; }
        if (slot < 0) return;
        int role = -2;
        for (int b = 0; b < NBTN; b++) {
            float dx = x - g_btn[b].x, dy = y - g_btn[b].y, rr = g_btn[b].r * 1.3f;
            if (dx * dx + dy * dy <= rr * rr) { role = b; break; }
        }
        if (role == -2 && x < 0.45f * g_w / g_h && x > g_inset_l * 0.5f && y > 0.30f) role = -1;
        if (role >= 0 && g_btn[role].toggle) g_latched[role] ^= 1;   /* press toggles crouch / senses */
        g_t[slot].used = 1; g_t[slot].id = id; g_t[slot].role = role;
        g_t[slot].x0 = g_t[slot].x = x; g_t[slot].y0 = g_t[slot].y = y;
        return;
    }
    for (int i = 0; i < MAXT; i++) {
        if (!g_t[i].used || g_t[i].id != id) continue;
        if (type == 1) { g_t[i].x = x; g_t[i].y = y; }
        else g_t[i].used = 0;
    }
}

static int16_t axis(float v) { v = v > 1 ? 1 : v < -1 ? -1 : v; return (int16_t)(v * 32767); }

void touch_apply(void) {
    if (!touch_enabled() || !g_layout_done) return;
    int down[NBTN] = {0};
    float sx = 0, sy = 0, cx = 0, cy = 0;
    for (int i = 0; i < MAXT; i++) {
        if (!g_t[i].used) continue;
        float dx = g_t[i].x - g_t[i].x0, dy = g_t[i].y - g_t[i].y0;
        if (g_t[i].role == -1) { sx = dx / 0.12f; sy = dy / 0.12f; }
        else if (g_t[i].role == -2) { cx = dx / 0.10f; cy = dy / 0.10f; }
        else down[g_t[i].role] = 1;
    }
    if (sx || sy) { g_pad.lx = axis(sx); g_pad.ly = axis(sy); }
    if (cx || cy) { g_pad.rx = axis(cx); g_pad.ry = axis(cy); }
    for (int b = 0; b < NBTN; b++) {
        int on = g_btn[b].toggle ? g_latched[b] : down[b];
        if (on) g_pad.buttons[g_btn[b].pad_button] = 0x80;
        g_down[b] = on;
    }
}

int touch_overlay(UIInst *o, int max, float w, float h) {
    if (!touch_enabled()) return 0;
    g_w = w; g_h = h;
    layout();
    int n = 0;
    for (int b = 0; b < NBTN && n < max; b++) {
        if (g_btn[b].r <= 0) continue;
        UIInst *u = &o[n++];
        memset(u, 0, sizeof *u);
        u->cx = g_btn[b].x * h; u->cy = g_btn[b].y * h; u->r = g_btn[b].r * h; u->pressed = (float)g_down[b];
        u->col[0] = g_btn[b].col[0]; u->col[1] = g_btn[b].col[1]; u->col[2] = g_btn[b].col[2]; u->col[3] = 0.6f;
        u->icon = (float)(g_btn[b].icon + 1);
    }
    for (int i = 0; i < MAXT && n + 2 <= max; i++) {   /* floating stick: base + knob */
        if (!g_t[i].used || g_t[i].role != -1) continue;
        float dx = g_t[i].x - g_t[i].x0, dy = g_t[i].y - g_t[i].y0, l = sqrtf(dx * dx + dy * dy), m = 0.12f;
        if (l > m) { dx *= m / l; dy *= m / l; }
        o[n++] = (UIInst){g_t[i].x0 * h, g_t[i].y0 * h, 0.15f * h, 0, {1, 1, 1, 0.35f}, 0, 0, 0, 0};
        o[n++] = (UIInst){(g_t[i].x0 + dx) * h, (g_t[i].y0 + dy) * h, 0.07f * h, 1, {1, 1, 1, 0.6f}, 0, 0, 0, 0};
    }
    return n;
}
