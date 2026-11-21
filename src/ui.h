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

struct InputContext;
/* Message shown on the launcher the next time it opens (e.g. why a stream
 * could not start).  Contains "ailed" → drawn in the error colour. */
void ui_set_launcher_notice(const char *text);

/* Over the running stream: "Put the console in rest mode?"  Returns true for
 * rest mode; Back or Home keeps it on. */
bool ui_ask_rest_mode(SDL_Renderer *renderer, struct InputContext *input);

/* Start screen: the console list (connect, add, settings per console, remove),
 * app settings and the controller test.  On UI_RESULT_CONNECT the chosen console
 * is the active one in cfg (config_select_console). */
UIResult ui_run_home(SDL_Renderer *renderer, AppConfig *cfg, const char *config_path,
                     ChiakiLog *log, struct InputContext *input);

/* App settings screen (controllers, overlay, logging). */
void ui_run_settings(SDL_Renderer *renderer, AppConfig *cfg, const char *config_path,
                     struct InputContext *input);

/* Gamepad activity keeps the TV's screensaver away for 2 minutes (webOS does
 * not see grabbed gamepads).  Call ui_keep_awake_tick() every frame. */
void ui_note_pad_activity(void);
void ui_keep_awake_tick(void);

/* The notice set by ui_set_launcher_notice(), once (then cleared). */
const char *ui_take_launcher_notice(void);

// Draw a semi-transparent stats overlay on top of the current frame.
// This draws, but does NOT call SDL_RenderPresent().
void ui_render_stats_overlay(SDL_Renderer *renderer, const char *text, int corner); /* 0 TL, 1 TR, 2 BL, 3 BR */

/* ── Menu input: TV remote keys + LG Magic Remote pointer ─────────────────
 * Back (remote Back, Escape, gamepad Circle) is one key everywhere:
 * ui_menu_key() returns SDLK_ESCAPE for it.  Pointer: each frame a screen
 * calls ui_hit_begin() (ui_draw_header does it) and ui_hit_add() for every
 * clickable thing with an id of its choice.  Moving the pointer onto one makes
 * ui_pointer_hover() return its id (the screen moves its focus there); a click
 * does the same and ui_menu_key() returns SDLK_RETURN.  The wheel scrolls
 * (Up/Down). */
#define UI_SCANCODE_WEBOS_BACK 482                          /* SDL_WEBOS_SCANCODE_BACK */
#define UI_KEY_WEBOS_BACK SDL_SCANCODE_TO_KEYCODE(UI_SCANCODE_WEBOS_BACK)
bool        ui_is_back_key(SDL_Keycode k);
bool        ui_is_back_event(const SDL_Event *ev);   /* any Back key press */
SDL_Keycode ui_menu_key(const SDL_Event *ev);
int         ui_pointer_hover(void);
bool        ui_pointer_clicked(void);    /* the last SDLK_RETURN came from a click */
void        ui_pointer_reset(void);      /* entering a screen: forget the hover */
/* Pointer visibility (hide on a direction key or after 8 s idle): feed every
 * SDL event to ui_pointer_event() (ui_menu_key() does) and call
 * ui_pointer_tick() every frame (pad_nav_poll() does). */
void        ui_pointer_event(const SDL_Event *ev);
void        ui_pointer_tick(void);
void        ui_hit_begin(void);
void        ui_hit_add(int id, int x, int y, int w, int h);

// Draw temporary pop up at launch for stats overlay
void ui_render_hint(SDL_Renderer *r, const char *text, float opacity);