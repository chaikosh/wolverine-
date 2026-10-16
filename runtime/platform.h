/* Host platform layer (SDL3): window, input state, timing. */
#pragma once
#include <stdint.h>

void plat_init(void);
void *plat_create_window(const char *title, int w, int h);  /* returns SDL_Window* */
void *plat_window(void);
void plat_window_size(int *w, int *h);                       /* drawable pixels */
int plat_pump(void);                                          /* pumps SDL events; returns 0 on quit */

/* keyboard state indexed by DirectInput scancode (DIK_*) */
extern uint8_t g_dik_state[256];
/* windows messages generated from host events */
typedef struct { uint32_t msg, wparam, lparam; } WinMsg;
int plat_pop_msg(WinMsg *m);
void plat_push_msg(uint32_t msg, uint32_t wparam, uint32_t lparam);

/* buffered keyboard events for DirectInput GetDeviceData */
typedef struct { uint8_t dik, down; uint32_t time; } KeyEvt;
int plat_pop_key(KeyEvt *e);
void plat_key(uint8_t dik, int down);          /* inject a key press/release (touch UI, scripts) */
void plat_native_mode(int *w, int *h);         /* display size in pixels, landscape */
void plat_rumble(float intensity, float seconds);   /* connected gamepad rumble */
void plat_safe_insets(float *left, float *right);   /* landscape safe-area insets, pixels */

/* mouse */
extern int32_t g_mouse_dx, g_mouse_dy, g_mouse_wheel;
extern uint8_t g_mouse_buttons[4];

/* gamepad (XInput-like layout, filled every pump) */
typedef struct {
    int connected;
    int16_t lx, ly, rx, ry;       /* -32768..32767, y down = positive */
    uint8_t lt, rt;               /* 0..255 */
    uint8_t buttons[16];          /* A B X Y LB RB Back Start LS RS DUp DDown DLeft DRight Guide Misc */
} Pad;
extern Pad g_pad;
