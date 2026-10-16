/* Rumble output: phone Taptic Engine (iOS) and connected gamepads. */
#pragma once
void haptics_play(float intensity, float seconds);   /* intensity 0..1; seconds <= 0 means "until stop" */
void haptics_stop(void);
