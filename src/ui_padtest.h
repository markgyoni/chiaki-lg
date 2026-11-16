#pragma once
/* Controller test screen: shows live input exactly as the console would get
 * it (buttons, sticks, triggers, touchpad, motion) and lets the user try the
 * outputs (vibration, adaptive triggers, lightbar, player LEDs).
 * Exit: Back/Red on the TV remote, or hold PS on a controller for 2 s. */

#include <SDL2/SDL.h>

struct InputContext;

void ui_run_padtest(SDL_Renderer *r, struct InputContext *input);
