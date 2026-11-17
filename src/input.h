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
    bool        trigger_effects;   // config "trigger_effects": DualSense adaptive triggers
    bool        dualsense_bt_patch; // config "dualsense_bt_patch": run btfix for BT DualSense outputs
} InputOptions;

InputContext *input_init(const InputOptions *opt);
void          input_fini(InputContext *ctx);

// Apply changed options (from the settings screen) without restarting: every
// gamepad is closed and reopened with the new profile / backend / outputs.
void          input_reconfigure(InputContext *ctx, const InputOptions *opt);

// ── For the launcher UI (menu navigation, controller list, gamepad test) ─────

// Current combined state of all gamepads, exactly as it would be sent to the
// console (buttons, sticks, triggers, touches, motion).  Excludes the TV remote.
void input_get_state(InputContext *ctx, ChiakiControllerState *out);

typedef struct {
    char name[64];
    char connection[32];     // "USB", "Bluetooth", "USB (Bluetooth on standby)"
    char profile[24];        // mapping profile id
    bool dualsense;          // DualSense / DualSense Edge
    bool raw_hid;            // read through raw HID (touchpad + motion)
    bool outputs_active;     // rumble/triggers/lightbar reach the pad
    char outputs[96];        // human-readable output status
} InputPadInfo;

// Fill up to `max` entries; returns the number of connected gamepads.
int  input_get_pads(InputContext *ctx, InputPadInfo *out, int max);

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

// DualSense outputs from the PS5 (all non-blocking; the input thread sends them).
void input_set_trigger_effects(InputContext *ctx, uint8_t type_l, const uint8_t l[10],
                               uint8_t type_r, const uint8_t r[10]);
void input_set_lightbar(InputContext *ctx, uint8_t r, uint8_t g, uint8_t b);
void input_set_player_index(InputContext *ctx, uint8_t index);
// triggers=false: vibration intensity.  level: 0 Off, 1 Strong, 2 Medium, 3 Weak.
void input_set_effect_intensity(InputContext *ctx, bool triggers, int level);

// Handle SDL keyboard events from the TV remote only.
// Gamepad buttons/axes are handled internally via direct evdev + EVIOCGRAB.
void input_handle_event(SDL_Event *ev, InputContext *ctx, ChiakiSession *session);
