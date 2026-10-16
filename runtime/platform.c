/* SDL3 host platform. */
#include "platform.h"
#include "runtime.h"
#include "touch.h"
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <stdlib.h>

uint8_t g_dik_state[256];
int32_t g_mouse_dx, g_mouse_dy, g_mouse_wheel;
uint8_t g_mouse_buttons[4];
Pad g_pad;

static SDL_Window *g_win;
static SDL_Gamepad *g_gp;
static KeyEvt g_kq[256];
static int g_kqh, g_kqt;
int plat_pop_key(KeyEvt *e) {
    if (g_kqh == g_kqt) return 0;
    *e = g_kq[g_kqh];
    g_kqh = (g_kqh + 1) & 255;
    return 1;
}
static void push_key(uint8_t dik, int down) {
    int n = (g_kqt + 1) & 255;
    if (n == g_kqh) return;
    g_kq[g_kqt] = (KeyEvt){dik, (uint8_t)down, (uint32_t)SDL_GetTicks()};
    g_kqt = n;
}
void plat_key(uint8_t dik, int down) {
    g_dik_state[dik] = down ? 0x80 : 0;
    push_key(dik, down);
    uint32_t vk = dik == 0x01 ? 0x1B : dik == 0x1C ? 0x0D : 0;
    if (vk) plat_push_msg(down ? 0x0100 : 0x0101, vk, ((uint32_t)dik << 16) | 1u);
}
static WinMsg g_q[256];
static int g_qh, g_qt;

void plat_push_msg(uint32_t msg, uint32_t w, uint32_t l) {
    int n = (g_qt + 1) & 255;
    if (n == g_qh) return;
    g_q[g_qt] = (WinMsg){msg, w, l};
    g_qt = n;
}
int plat_pop_msg(WinMsg *m) {
    if (g_qh == g_qt) return 0;
    *m = g_q[g_qh];
    g_qh = (g_qh + 1) & 255;
    return 1;
}

/* SDL scancode -> DirectInput DIK code (PC set 1) */
static uint8_t dik_of(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
    }
}
/* DIK -> Windows virtual key (enough for WM_KEYDOWN handlers) */
static uint32_t vk_of(SDL_Keycode k, uint8_t dik) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 32);
    if (k >= '0' && k <= '9') return (uint32_t)k;
    switch (dik) {
    case 0x01: return 0x1B; case 0x1C: return 0x0D; case 0x39: return 0x20; case 0x0E: return 0x08; case 0x0F: return 0x09;
    case 0xC8: return 0x26; case 0xD0: return 0x28; case 0xCB: return 0x25; case 0xCD: return 0x27;
    case 0x2A: case 0x36: return 0x10; case 0x1D: case 0x9D: return 0x11; case 0x38: case 0xB8: return 0x12;
    }
    if (dik >= 0x3B && dik <= 0x44) return 0x70u + (dik - 0x3Bu);
    return 0;
}

void plat_init(void) {
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) LOG("[sdl] init failed: %s\n", SDL_GetError());
}

void *plat_create_window(const char *title, int w, int h) {
    if (g_win) return g_win;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    g_win = SDL_CreateWindow(title, w, h, SDL_WINDOW_METAL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
#else
    g_win = SDL_CreateWindow(title, w, h, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#endif
    if (!g_win) LOG("[sdl] window failed: %s\n", SDL_GetError());
    return g_win;
}
void *plat_window(void) { return g_win; }
/* Left/right safe-area insets in pixels (Dynamic Island / rounded corners in landscape). */
void plat_safe_insets(float *left, float *right) {
    *left = *right = 0;
    if (!g_win) return;
    SDL_Rect r; int w, h;
    if (!SDL_GetWindowSafeArea(g_win, &r)) return;
    SDL_GetWindowSize(g_win, &w, &h);
    float dens = SDL_GetWindowPixelDensity(g_win);
    *left = (float)r.x * dens;
    *right = (float)(w - (r.x + r.w)) * dens;
}
void plat_rumble(float intensity, float seconds) {
    if (!g_gp) return;
    uint16_t v = (uint16_t)(intensity * 65535.f);
    uint32_t ms = seconds <= 0 ? (intensity > 0 ? 2000u : 0u) : (uint32_t)(seconds * 1000.f);
    SDL_RumbleGamepad(g_gp, v, (uint16_t)(v / 2), ms);
}
/* Pad buttons that also act as keys: Start/Back = Esc (pause/back), A = Enter (confirm prompts). */
static void pad_keys(void) {
    static uint8_t prev[16];
    static const struct { int b; uint8_t dik; } map[] = {{7, 0x01}, {6, 0x01}, {0, 0x1C}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        int now = g_pad.buttons[map[i].b] != 0;
        if (now != (prev[map[i].b] != 0)) plat_key(map[i].dik, now);
    }
    memcpy(prev, g_pad.buttons, sizeof prev);
}
void plat_native_mode(int *w, int *h) {
    *w = *h = 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return;
    int a = (int)(m->w * m->pixel_density), b = (int)(m->h * m->pixel_density);
    *w = a > b ? a : b; *h = a > b ? b : a;
}
void plat_window_size(int *w, int *h) { SDL_GetWindowSizeInPixels(g_win, w, h); }

static void update_pad(void) {
    if (!g_gp) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) { g_gp = SDL_OpenGamepad(ids[0]); if (g_gp) LOG("[pad] %s connected\n", SDL_GetGamepadName(g_gp)); }
        SDL_free(ids);
    }
    memset(&g_pad, 0, sizeof g_pad);
    if (!g_gp) return;
    g_pad.connected = 1;
    g_pad.lx = SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_LEFTX);
    g_pad.ly = SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_LEFTY);
    g_pad.rx = SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_RIGHTX);
    g_pad.ry = SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_RIGHTY);
    g_pad.lt = (uint8_t)(SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    g_pad.rt = (uint8_t)(SDL_GetGamepadAxis(g_gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    static const SDL_GamepadButton map[15] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_GUIDE};
    for (int i = 0; i < 15; i++) g_pad.buttons[i] = SDL_GetGamepadButton(g_gp, map[i]) ? 0x80 : 0;
    /* LT = crouch, RT = senses, LB = stealth: press toggles on, press again toggles off */
    static int lt_prev, rt_prev, lb_prev, lt_on, rt_on, lb_on;
    int lt = g_pad.lt > 96, rt = g_pad.rt > 96, lb = g_pad.buttons[4] != 0;
    if (lt && !lt_prev) lt_on ^= 1;
    if (rt && !rt_prev) rt_on ^= 1;
    if (lb && !lb_prev) lb_on ^= 1;
    lt_prev = lt; rt_prev = rt; lb_prev = lb;
    g_pad.buttons[10] = lt_on ? 0x80 : 0;
    g_pad.buttons[11] = rt_on ? 0x80 : 0;
    g_pad.buttons[4] = lb_on ? 0x80 : 0;
}

/* Scripted input for automated tests: AUTOKEYS="sec:dik[:hold],..." presses a DIK key (default 150 ms). */
static void autokeys(void) {
    static int n = -1; static float at[64], hold[64]; static int key[64]; static int state[64];
    static uint64_t t0;
    if (n < 0) {
        n = 0; t0 = SDL_GetTicks();
        const char *e = getenv("AUTOKEYS");
        while (e && *e && n < 64) {
            at[n] = strtof(e, (char **)&e); if (*e == ':') e++;
            key[n] = (int)strtol(e, (char **)&e, 0);
            hold[n] = 0.15f;
            if (*e == ':') { e++; hold[n] = strtof(e, (char **)&e); }
            if (*e == ',') e++;
            n++;
        }
    }
    float t = (SDL_GetTicks() - t0) / 1000.f;
    for (int i = 0; i < n; i++) {
        uint8_t d = (uint8_t)key[i];
        if (state[i] == 0 && t >= at[i]) { state[i] = 1; g_dik_state[d] = 0x80; push_key(d, 1); LOG("[auto] key %02X down at %.1fs\n", d, t); }
        else if (state[i] == 1 && t >= at[i] + hold[i]) { state[i] = 2; g_dik_state[d] = 0; push_key(d, 0); }
    }
}

/* Scripted gamepad for tests: AUTOPAD="sec:button[:hold],..." (0-15 buttons, 20=stick up, 21=stick down). */
static void autopad(void) {
    static int n = -1; static float at[64], hold[64]; static int btn[64];
    static uint64_t t0;
    if (n < 0) {
        n = 0; t0 = SDL_GetTicks();
        const char *e = getenv("AUTOPAD");
        while (e && *e && n < 64) {
            at[n] = strtof(e, (char **)&e); if (*e == ':') e++;
            btn[n] = (int)strtol(e, (char **)&e, 0);
            hold[n] = 0.15f;
            if (*e == ':') { e++; hold[n] = strtof(e, (char **)&e); }
            if (*e == ',') e++;
            n++;
        }
    }
    float t = (SDL_GetTicks() - t0) / 1000.f;
    for (int i = 0; i < n; i++) {
        if (t < at[i] || t >= at[i] + hold[i]) continue;
        g_pad.connected = 1;
        if (btn[i] < 16) g_pad.buttons[btn[i]] = 0x80;
        else if (btn[i] == 20) g_pad.ly = -32767;
        else if (btn[i] == 21) g_pad.ly = 32767;
    }
}

int plat_pump(void) {
    SDL_Event e;
    int alive = 1;
    autokeys();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            plat_push_msg(0x0010, 0, 0);   /* WM_CLOSE */
            alive = 0;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint8_t d = dik_of(e.key.scancode);
            int down = e.type == SDL_EVENT_KEY_DOWN;
            if (d) { g_dik_state[d] = down ? 0x80 : 0; if (!e.key.repeat) push_key(d, down); }
            uint32_t vk = vk_of(e.key.key, d);
            if (vk && !e.key.repeat) plat_push_msg(down ? 0x0100 : 0x0101, vk, ((uint32_t)d << 16) | 1u);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: g_mouse_dx += (int32_t)e.motion.xrel; g_mouse_dy += (int32_t)e.motion.yrel; break;
        case SDL_EVENT_MOUSE_WHEEL: g_mouse_wheel += (int32_t)(e.wheel.y * 120); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            g_mouse_buttons[b] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0x80 : 0;
            break;
        }
        case SDL_EVENT_FINGER_DOWN: touch_event(0, e.tfinger.fingerID, e.tfinger.x, e.tfinger.y); break;
        case SDL_EVENT_FINGER_MOTION: touch_event(1, e.tfinger.fingerID, e.tfinger.x, e.tfinger.y); break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: touch_event(2, e.tfinger.fingerID, e.tfinger.x, e.tfinger.y); break;
        case SDL_EVENT_GAMEPAD_REMOVED: if (g_gp) { SDL_CloseGamepad(g_gp); g_gp = NULL; } break;
        default: break;
        }
    }
    update_pad();
    touch_apply();
    autopad();
    pad_keys();
    return alive;
}
