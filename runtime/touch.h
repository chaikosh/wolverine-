/* On-screen touch controls feeding the virtual gamepad. */
#pragma once
#include <stdint.h>

typedef struct { float cx, cy, r, pressed; float col[4]; uint32_t g[4]; float nch, p0, p1, p2; } UIInst;   /* pixels, 64 bytes */

void touch_event(int type, uint64_t finger, float nx, float ny);   /* type: 0 down, 1 move, 2 up */
void touch_apply(void);                                            /* merge touch state into g_pad / keys */
int touch_overlay(UIInst *out, int max, float w, float h);         /* instances to draw this frame */
int touch_enabled(void);
