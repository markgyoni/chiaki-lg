#pragma once
/* DualSense raw HID input parsing (Stage 4 of ROADMAP.md).
 *
 * On webOS the DualSense runs on hid-generic, which only maps the basic
 * buttons/axes to evdev.  Touchpad, motion sensors and every output need the
 * raw reports from /dev/hidrawN.  Layout follows Linux hid-playstation's
 * struct dualsense_input_report:
 *   USB  report 0x01, 64 bytes: payload at offset 1
 *   BT   report 0x31, 78 bytes: payload at offset 2 (byte 1 = seq/tag)
 *   BT   report 0x01, 10 bytes: "simple" mode before the pad is switched to
 *        full reports (sticks, buttons, triggers only) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS_VENDOR          0x054c
#define DS_PRODUCT         0x0ce6
#define DS_EDGE_PRODUCT    0x0df2
#define DS_TOUCHPAD_W      1920
#define DS_TOUCHPAD_H      1080

typedef struct {
    bool     active;
    uint8_t  id;        /* 7-bit contact id from the pad */
    uint16_t x, y;      /* 0..1919, 0..1079 */
} DsTouch;

typedef struct {
    bool     full;                  /* full report: motion/touch fields valid */
    uint8_t  lx, ly, rx, ry;        /* 0..255, 128 = centre, y down */
    uint8_t  l2, r2;                /* 0..255 */
    uint32_t buttons;               /* CHIAKI_CONTROLLER_BUTTON_* incl. d-pad */
    bool     mute;
    int16_t  gyro[3];               /* raw, uncalibrated (pitch, yaw, roll) */
    int16_t  accel[3];              /* raw, uncalibrated (x, y, z) */
    uint32_t sensor_timestamp;      /* 0.33 us units */
    DsTouch  touch[2];
} DsInputState;

/* Parse one report read from hidraw.  Returns false for reports that are not
 * input state (other ids/sizes), leaving *out untouched. */
bool ds_parse_input(const uint8_t *rep, size_t len, DsInputState *out);

bool ds_is_dualsense(uint16_t vendor, uint16_t product);
