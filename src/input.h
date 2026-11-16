#pragma once

#include <SDL2/SDL.h>
#include <chiaki/session.h>
#include <chiaki/controller.h>

typedef struct InputContext InputContext;

typedef struct {
    bool        debug;             // config "input_debug": log raw events, sample hidraw
    const char *profile_override;  // config "controller_profile": "auto" or a pad_profile id
    bool        rumble;            // config "rumble"
    bool        dualsense_hid;     // config "dualsense_hid": raw HID backend for DualSense
    bool        motion;            // config "motion": forward DualSense gyro/accel
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

// One frame of PS5 haptics audio (DualSense mode: interleaved stereo int16 PCM).
// Converted to rumble the way chiaki-ng does for non-DualSense controllers.
// Non-blocking; called from chiaki's audio thread.
void input_push_haptics(InputContext *ctx, const uint8_t *buf, size_t buf_size);

// CHIAKI_EVENT_MOTION_RESET: re-zero the orientation of motion-capable pads.
void input_motion_reset(InputContext *ctx);

// Handle SDL keyboard events from the TV remote only.
// Gamepad buttons/axes are handled internally via direct evdev + EVIOCGRAB.
void input_handle_event(SDL_Event *ev, InputContext *ctx, ChiakiSession *session);
