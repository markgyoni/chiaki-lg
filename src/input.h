#pragma once

#include <SDL2/SDL.h>
#include <chiaki/session.h>
#include <chiaki/controller.h>

typedef struct InputContext InputContext;

typedef struct {
    bool        debug;             // config "input_debug": log raw events, sample hidraw
    const char *profile_override;  // config "controller_profile": "auto" or a pad_profile id
    bool        rumble;            // config "rumble"
} InputOptions;

InputContext *input_init(const InputOptions *opt);
void          input_fini(InputContext *ctx);

// Set the active chiaki session so the evdev reader thread can push
// controller state updates directly (bypassing SDL's joystick subsystem).
// Call this after chiaki_session_start() succeeds; pass NULL before
// chiaki_session_join() — no send is in flight once it returns.  Clears rumble.
void input_set_session(InputContext *ctx, ChiakiSession *session);

// Rumble request from the PS5 (CHIAKI_EVENT_RUMBLE): strong = left/low-frequency
// motor, weak = right/high-frequency motor, 0-255.  Non-blocking; safe to call
// from chiaki's callback thread — the input thread drives the motors.
void input_set_rumble(InputContext *ctx, uint8_t strong, uint8_t weak);

// Handle SDL keyboard events from the TV remote only.
// Gamepad buttons/axes are handled internally via direct evdev + EVIOCGRAB.
void input_handle_event(SDL_Event *ev, InputContext *ctx, ChiakiSession *session);
