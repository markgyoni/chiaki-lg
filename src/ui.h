#pragma once

#include <SDL2/SDL.h>
#include <chiaki/log.h>
#include "config.h"

typedef enum {
    UI_RESULT_CONNECT,      // user chose to connect
    UI_RESULT_QUIT,         // user dismissed the setup/launcher screen
    UI_RESULT_ERROR,
} UIResult;

// Minimal fullscreen loading screen (SDL renderer).
// Call once per tick while you're doing blocking startup work.
// This draws, but does NOT call SDL_RenderPresent().
void ui_render_loading(SDL_Renderer *renderer, const char *base_text);

// Show the launcher/setup screen.
// Displayed on every launch. Lets the user:
//  - enter PS5 IP address
//  - import chiaki-ng config (chiaki-ng-Default.ini) into config.json
//  - connect to start the stream
struct InputContext;
/* Message shown on the launcher the next time it opens (e.g. why a stream
 * could not start).  Contains "ailed" → drawn in the error colour. */
void ui_set_launcher_notice(const char *text);

/* Over the running stream: "Put the console in rest mode?"  Returns true for
 * rest mode; Back, no answer within 15 s or Home keeps it on. */
bool ui_ask_rest_mode(SDL_Renderer *renderer, struct InputContext *input);

UIResult ui_run_registration(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, ChiakiLog *log,
                             struct InputContext *input);

// Draw a semi-transparent stats overlay on top of the current frame.
// This draws, but does NOT call SDL_RenderPresent().
void ui_render_stats_overlay(SDL_Renderer *renderer, const char *text, int corner); /* 0 TL, 1 TR, 2 BL, 3 BR */

// Draw temporary pop up at launch for stats overlay
void ui_render_hint(SDL_Renderer *r, const char *text, float opacity);