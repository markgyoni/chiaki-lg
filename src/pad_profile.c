#include "pad_profile.h"

#include <stdio.h>
#include <string.h>

#include <chiaki/controller.h>
#include <linux/input.h>

/* evdev codes by value: the webOS sysroot ships 3.10 kernel headers that
 * lack the BTN_SOUTH/... aliases and BTN_DPAD_*. */
enum {
    K_SOUTH = 0x130, K_EAST = 0x131, K_C = 0x132, K_NORTH = 0x133, K_WEST = 0x134,
    K_Z = 0x135, K_TL = 0x136, K_TR = 0x137, K_TL2 = 0x138, K_TR2 = 0x139,
    K_SELECT = 0x13a, K_START = 0x13b, K_MODE = 0x13c, K_THUMBL = 0x13d, K_THUMBR = 0x13e,
    K_RECORD = 0x167,
    K_DPAD_UP = 0x220, K_DPAD_DOWN = 0x221, K_DPAD_LEFT = 0x222, K_DPAD_RIGHT = 0x223,
};
enum {
    A_X = 0x00, A_Y = 0x01, A_Z = 0x02, A_RX = 0x03, A_RY = 0x04, A_RZ = 0x05,
    A_GAS = 0x09, A_BRAKE = 0x0a, A_HAT0X = 0x10, A_HAT0Y = 0x11,
};

#define B_CROSS    CHIAKI_CONTROLLER_BUTTON_CROSS
#define B_CIRCLE   CHIAKI_CONTROLLER_BUTTON_MOON
#define B_SQUARE   CHIAKI_CONTROLLER_BUTTON_BOX
#define B_TRIANGLE CHIAKI_CONTROLLER_BUTTON_PYRAMID
#define B_L1       CHIAKI_CONTROLLER_BUTTON_L1
#define B_R1       CHIAKI_CONTROLLER_BUTTON_R1
#define B_L3       CHIAKI_CONTROLLER_BUTTON_L3
#define B_R3       CHIAKI_CONTROLLER_BUTTON_R3
#define B_SHARE    CHIAKI_CONTROLLER_BUTTON_SHARE
#define B_OPTIONS  CHIAKI_CONTROLLER_BUTTON_OPTIONS
#define B_PS       CHIAKI_CONTROLLER_BUTTON_PS
#define B_TOUCH    CHIAKI_CONTROLLER_BUTTON_TOUCHPAD

#define DPAD_BUTTONS \
    { K_DPAD_UP,    CHIAKI_CONTROLLER_BUTTON_DPAD_UP    }, \
    { K_DPAD_DOWN,  CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN  }, \
    { K_DPAD_LEFT,  CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT  }, \
    { K_DPAD_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT }

#define HAT_AXES { A_HAT0X, PAD_AXIS_HATX }, { A_HAT0Y, PAD_AXIS_HATY }

/* DualSense / DualShock 4 on hid-generic: buttons arrive in HID descriptor
 * order (Square, Cross, Circle, Triangle, L1, R1, L2, R2, Create, Options,
 * L3, R3, PS, Touchpad, Mute) and hid-input numbers them from BTN_SOUTH.
 * Triggers are analog on RX/RY, so the digital L2/R2 bits are ignored.
 * Verified on webOS 6 — docs/controllers/dualsense-bt-webos6.md. */
static const PadProfile PROFILE_SONY_GENERIC = {
    .id = "sony_generic",
    .description = "PlayStation pad on hid-generic",
    .chord_touchpad = false,
    .buttons = {
        { K_SOUTH,  B_SQUARE   }, { K_EAST,   B_CROSS    }, { K_C,      B_CIRCLE },
        { K_NORTH,  B_TRIANGLE }, { K_WEST,   B_L1       }, { K_Z,      B_R1     },
        { K_TL2,    B_SHARE    }, { K_TR2,    B_OPTIONS  },
        { K_SELECT, B_L3       }, { K_START,  B_R3       }, { K_MODE,   B_PS     },
        { K_THUMBL, B_TOUCH    },
        /* K_TL / K_TR: digital L2/R2, superseded by analog RX/RY.
         * K_THUMBR: mic mute, no PS equivalent. */
        DPAD_BUTTONS,
        { 0, 0 },
    },
    .axes = {
        { A_X, PAD_AXIS_LX }, { A_Y, PAD_AXIS_LY },
        { A_Z, PAD_AXIS_RX }, { A_RZ, PAD_AXIS_RY },
        { A_RX, PAD_AXIS_L2 }, { A_RY, PAD_AXIS_R2 },
        HAT_AXES,
        { 0, PAD_AXIS_NONE },
    },
};

/* DualShock 4 / DualSense on a driver that remaps to the Linux gamepad
 * layout (hid-sony ≥ 4.10, hid-playstation): positional face buttons,
 * L2/R2 analog on Z/RZ, right stick on RX/RY.  Not yet seen on a TV. */
static const PadProfile PROFILE_SONY_KERNEL = {
    .id = "sony_kernel",
    .description = "PlayStation pad on hid-sony/hid-playstation",
    .chord_touchpad = false,
    .buttons = {
        { K_SOUTH,  B_CROSS    }, { K_EAST,   B_CIRCLE   },
        { K_NORTH,  B_TRIANGLE }, { K_WEST,   B_SQUARE   },
        { K_TL,     B_L1       }, { K_TR,     B_R1       },
        { K_SELECT, B_SHARE    }, { K_START,  B_OPTIONS  }, { K_MODE, B_PS },
        { K_THUMBL, B_L3       }, { K_THUMBR, B_R3       },
        DPAD_BUTTONS,
        { 0, 0 },
    },
    .axes = {
        { A_X, PAD_AXIS_LX }, { A_Y, PAD_AXIS_LY },
        { A_RX, PAD_AXIS_RX }, { A_RY, PAD_AXIS_RY },
        { A_Z, PAD_AXIS_L2 }, { A_RZ, PAD_AXIS_R2 },
        HAT_AXES,
        { 0, PAD_AXIS_NONE },
    },
};

/* Xbox Wireless over Bluetooth (hid-generic / hid-microsoft).  Mapping kept
 * from the original input.c, which was tested with this controller:
 * the descriptor has gaps, so A/B/X/Y land on 0x130/0x131/0x133/0x134,
 * right stick on Z/RZ, triggers on GAS (RT) / BRAKE (LT). */
static const PadProfile PROFILE_XBOX_BT = {
    .id = "xbox_bt",
    .description = "Xbox pad over Bluetooth",
    .chord_touchpad = true,
    .buttons = {
        { K_SOUTH,  B_CROSS    }, { K_EAST,   B_CIRCLE   },
        { K_NORTH,  B_SQUARE   }, { K_WEST,   B_TRIANGLE },
        { K_TL,     B_L1       }, { K_TR,     B_R1       },
        { K_SELECT, B_SHARE    }, { K_START,  B_OPTIONS  }, { K_MODE, B_PS },
        { K_THUMBL, B_L3       }, { K_THUMBR, B_R3       },
        { K_RECORD, B_TOUCH    },
        DPAD_BUTTONS,
        { 0, 0 },
    },
    .axes = {
        { A_X, PAD_AXIS_LX }, { A_Y, PAD_AXIS_LY },
        { A_Z, PAD_AXIS_RX }, { A_RZ, PAD_AXIS_RY },
        { A_BRAKE, PAD_AXIS_L2 }, { A_GAS, PAD_AXIS_R2 },
        HAT_AXES,
        { 0, PAD_AXIS_NONE },
    },
};

/* Xbox / XInput pads on the xpad driver (USB): BTN_X (0x133) is the left
 * face button, BTN_Y (0x134) the top one; triggers on Z/RZ. */
static const PadProfile PROFILE_XPAD = {
    .id = "xpad",
    .description = "Xbox/XInput pad on xpad (USB)",
    .chord_touchpad = true,
    .buttons = {
        { K_SOUTH,  B_CROSS    }, { K_EAST,   B_CIRCLE   },
        { K_NORTH,  B_SQUARE   }, { K_WEST,   B_TRIANGLE },
        { K_TL,     B_L1       }, { K_TR,     B_R1       },
        { K_TL2,    PAD_BTN_L2_DIGITAL }, { K_TR2, PAD_BTN_R2_DIGITAL },
        { K_SELECT, B_SHARE    }, { K_START,  B_OPTIONS  }, { K_MODE, B_PS },
        { K_THUMBL, B_L3       }, { K_THUMBR, B_R3       },
        { K_RECORD, B_TOUCH    },
        DPAD_BUTTONS,
        { 0, 0 },
    },
    .axes = {
        { A_X, PAD_AXIS_LX }, { A_Y, PAD_AXIS_LY },
        { A_RX, PAD_AXIS_RX }, { A_RY, PAD_AXIS_RY },
        { A_Z, PAD_AXIS_L2 }, { A_RZ, PAD_AXIS_R2 },
        HAT_AXES,
        { 0, PAD_AXIS_NONE },
    },
};

static const PadProfile *const ALL_PROFILES[] = {
    &PROFILE_SONY_GENERIC, &PROFILE_SONY_KERNEL, &PROFILE_XBOX_BT, &PROFILE_XPAD,
};
#define N_PROFILES (sizeof(ALL_PROFILES) / sizeof(ALL_PROFILES[0]))

#define BIT(arr, b) ((arr) && (((arr)[(b) / 8]) & (1u << ((b) % 8))))

static bool has_abs(const PadDeviceInfo *d, int code) { return BIT(d->absbits, code); }
static bool has_key(const PadDeviceInfo *d, int code) { return BIT(d->keybits, code); }

/* A trigger rests at its minimum; a stick rests mid-range. */
static bool rests_at_min(const PadDeviceInfo *d, int code)
{
    return d->abs_max[code] - d->abs_min[code] > 2 &&
           d->abs_value[code] <= d->abs_min[code] + (d->abs_max[code] - d->abs_min[code]) / 8;
}

/* Fallback for unknown pads: Linux gamepad-API style buttons (xpad layout)
 * with axes chosen by what the device advertises and where they rest. */
static PadProfile make_generic(const PadDeviceInfo *d)
{
    PadProfile p = PROFILE_XPAD;
    p.id = "generic";
    p.description = "Unknown pad (heuristic axes)";

    int n = 0;
    p.axes[n++] = (PadAxisMap){ A_X, PAD_AXIS_LX };
    p.axes[n++] = (PadAxisMap){ A_Y, PAD_AXIS_LY };
    if (has_abs(d, A_GAS) || has_abs(d, A_BRAKE)) {
        p.axes[n++] = (PadAxisMap){ A_Z, PAD_AXIS_RX };
        p.axes[n++] = (PadAxisMap){ A_RZ, PAD_AXIS_RY };
        p.axes[n++] = (PadAxisMap){ A_BRAKE, PAD_AXIS_L2 };
        p.axes[n++] = (PadAxisMap){ A_GAS, PAD_AXIS_R2 };
    } else if (has_abs(d, A_RX) && has_abs(d, A_RY) && has_abs(d, A_Z) && has_abs(d, A_RZ)
               && rests_at_min(d, A_RX) && !rests_at_min(d, A_Z)) {
        p.axes[n++] = (PadAxisMap){ A_Z, PAD_AXIS_RX };
        p.axes[n++] = (PadAxisMap){ A_RZ, PAD_AXIS_RY };
        p.axes[n++] = (PadAxisMap){ A_RX, PAD_AXIS_L2 };
        p.axes[n++] = (PadAxisMap){ A_RY, PAD_AXIS_R2 };
    } else if (has_abs(d, A_RX) && has_abs(d, A_RY)) {
        p.axes[n++] = (PadAxisMap){ A_RX, PAD_AXIS_RX };
        p.axes[n++] = (PadAxisMap){ A_RY, PAD_AXIS_RY };
        p.axes[n++] = (PadAxisMap){ A_Z, PAD_AXIS_L2 };
        p.axes[n++] = (PadAxisMap){ A_RZ, PAD_AXIS_R2 };
    } else {
        p.axes[n++] = (PadAxisMap){ A_Z, PAD_AXIS_RX };
        p.axes[n++] = (PadAxisMap){ A_RZ, PAD_AXIS_RY };
    }
    p.axes[n++] = (PadAxisMap){ A_HAT0X, PAD_AXIS_HATX };
    p.axes[n++] = (PadAxisMap){ A_HAT0Y, PAD_AXIS_HATY };
    p.axes[n] = (PadAxisMap){ 0, PAD_AXIS_NONE };
    return p;
}

PadProfile pad_profile_select(const PadDeviceInfo *d, const char *override)
{
    if (override && override[0] && strcmp(override, "auto") != 0) {
        for (size_t i = 0; i < N_PROFILES; i++)
            if (strcmp(ALL_PROFILES[i]->id, override) == 0)
                return *ALL_PROFILES[i];
        if (strcmp(override, "generic") == 0)
            return make_generic(d);
        /* unknown value: fall through to autodetect */
    }

    const char *drv = d->driver ? d->driver : "";

    if (d->vendor == 0x054c) {
        /* hid-generic numbering exposes BTN_C (Circle); remapping drivers never do. */
        if (has_key(d, K_C) || strcmp(drv, "hid-generic") == 0)
            return PROFILE_SONY_GENERIC;
        return PROFILE_SONY_KERNEL;
    }
    if (strcmp(drv, "xpad") == 0)
        return PROFILE_XPAD;
    if (d->vendor == 0x045e) {
        if (has_abs(d, A_GAS) || has_abs(d, A_BRAKE))
            return PROFILE_XBOX_BT;
        return PROFILE_XPAD;
    }
    return make_generic(d);
}

uint32_t pad_profile_button(const PadProfile *p, uint16_t code)
{
    for (int i = 0; i < PAD_MAX_BUTTONS && p->buttons[i].code; i++)
        if (p->buttons[i].code == code)
            return p->buttons[i].button;
    return 0;
}

PadAxisRole pad_profile_axis(const PadProfile *p, uint16_t code)
{
    for (int i = 0; i < PAD_MAX_AXES && p->axes[i].role != PAD_AXIS_NONE; i++)
        if (p->axes[i].code == code)
            return p->axes[i].role;
    return PAD_AXIS_NONE;
}

const char *pad_profile_ids(void)
{
    return "auto, sony_generic, sony_kernel, xbox_bt, xpad, generic";
}
