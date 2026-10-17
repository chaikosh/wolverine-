/* Pictograms for the touch buttons (what each button does), rendered once into a grayscale atlas. */
#pragma once
#include <stdint.h>

enum { ICON_PUNCH, ICON_KICK, ICON_JUMP, ICON_ACTION, ICON_CLAWS, ICON_CROUCH, ICON_SENSES, ICON_STEALTH,
       ICON_CAMERA, ICON_PAUSE, ICON_COUNT };
#define ICON_COLS 4
#define ICON_ROWS 3

/* Fills a (cell*ICON_COLS) x (cell*ICON_ROWS) 8-bit buffer, top row first; 255 = ink, 0 = empty. */
void icons_render(uint8_t *px, int cell);
