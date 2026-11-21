#pragma once
/* Per-controller mapping profiles: evdev codes → chiaki buttons/axes.
 *
 * The same evdev code means different physical buttons depending on the
 * kernel driver (e.g. a DualSense on hid-generic reports Square as
 * BTN_SOUTH), so each driver/controller family gets its own table.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAD_AXIS_NONE = 0,
    PAD_AXIS_LX, PAD_AXIS_LY,
    PAD_AXIS_RX, PAD_AXIS_RY,
    PAD_AXIS_L2, PAD_AXIS_R2,
    PAD_AXIS_HATX, PAD_AXIS_HATY,
} PadAxisRole;

/* Pseudo-buttons, outside the chiaki button bitmask range. */
#define PAD_BTN_L2_DIGITAL   (1u << 30)  /* digital-only trigger → l2_state 0/255 */
#define PAD_BTN_R2_DIGITAL   (1u << 31)

typedef struct { uint16_t code; uint32_t button; } PadButtonMap;  /* button: CHIAKI_CONTROLLER_BUTTON_* or PAD_BTN_* */
typedef struct { uint16_t code; PadAxisRole role; } PadAxisMap;

#define PAD_MAX_BUTTONS 24
#define PAD_MAX_AXES    10

typedef struct {
    const char  *id;              /* config value, e.g. "sony_generic" */
    const char  *description;
    bool         chord_touchpad;  /* Select+Start chord → Touchpad (pads without a touchpad) */
    PadButtonMap buttons[PAD_MAX_BUTTONS];  /* terminated by code 0 */
    PadAxisMap   axes[PAD_MAX_AXES];        /* terminated by role NONE */
} PadProfile;

/* Information about an opened evdev device used to pick a profile. */
typedef struct {
    uint16_t bustype, vendor, product;
    const char *driver;              /* "" if none */
    const uint8_t *keybits;          /* EVIOCGBIT(EV_KEY) */
    const uint8_t *absbits;          /* EVIOCGBIT(EV_ABS) */
    const int *abs_min, *abs_max, *abs_value;  /* indexed by ABS code */
} PadDeviceInfo;

/* Pick a profile.  `override` is the "controller_profile" config value;
 * NULL, "" or "auto" means autodetect.  Never returns NULL.  The returned
 * profile may be a per-device copy (for the generic fallback), so callers
 * store the struct by value. */
PadProfile pad_profile_select(const PadDeviceInfo *dev, const char *override);

/* Look-ups on a profile; return 0 / PAD_AXIS_NONE if unmapped. */
uint32_t    pad_profile_button(const PadProfile *p, uint16_t code);
PadAxisRole pad_profile_axis(const PadProfile *p, uint16_t code);

/* Comma-separated list of profile ids, for logs/UI. */
const char *pad_profile_ids(void);
