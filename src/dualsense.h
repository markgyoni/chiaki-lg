#pragma once
/* DualSense raw HID input parsing.
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

/* ── Motion ──────────────────────────────────────────────────────── */

#include <chiaki/orientation.h>

/* Factory IMU calibration from feature report 0x05 (hid-playstation layout). */
typedef struct {
    bool valid;
    int  gyro_bias[3], gyro_numer[3], gyro_denom[3];   /* → 1/1024 deg/s */
    int  acc_bias[3],  acc_numer[3],  acc_denom[3];    /* → 1/8192 g */
} DsCalib;

bool ds_parse_calibration(const uint8_t *buf, size_t len, DsCalib *c);

typedef struct {
    DsCalib  cal;
    /* Runtime gyro bias (raw units) used when no factory calibration is
     * available — on webOS the calibration request fails (uhid EIO).  Updated
     * whenever the pad has been still for ~1 s. */
    float    auto_bias[3];
    bool     auto_bias_valid;
    int      still_count;
    long     still_sum[3];
    int16_t  still_ref[3];

    bool     have_ts;
    uint32_t last_ts;
    uint64_t ts_us;
    float    last_accel[3];

    ChiakiOrientationTracker tracker;
    ChiakiAccelNewZero       accel_zero;
} DsMotion;

void ds_motion_init(DsMotion *m, const DsCalib *cal);
/* Feed one full report; writes gyro (rad/s), accel (g) and orientation into
 * the controller state. */
void ds_motion_update(DsMotion *m, const DsInputState *s, ChiakiControllerState *st);
/* CHIAKI_EVENT_MOTION_RESET: re-zero orientation around the current pose. */
void ds_motion_reset(DsMotion *m);

/* ── Outputs: rumble, adaptive triggers, lightbar, player LEDs ──── */

/* Minimum gap between two output reports.  On webOS Bluetooth two reports
 * landing a few ms apart wedged the pad's input stream until it was power-
 * cycled; one report per input-thread tick (30 ms) is plenty for rumble. */
#define DS_OUTPUT_MIN_INTERVAL_BT_MS   25
#define DS_OUTPUT_MIN_INTERVAL_USB_MS   8

typedef struct {
    int      fd;            /* hidraw, borrowed */
    bool     bt;            /* Bluetooth: report 0x31 + CRC32; USB: report 0x02 */
    uint8_t  seq;
    long     last_write_ms; /* CLOCK_MONOTONIC of the last report sent, 0 = never */

    bool     setup_done;    /* lightbar fade-out sent (takes over from firmware) */

    uint8_t  motor_strong, motor_weak;      bool rumble_dirty;
    uint8_t  trig_l[11], trig_r[11];        bool trig_dirty;   /* [0] mode, [1..10] params */
    uint8_t  intensity;                     bool intensity_dirty; /* hi nibble trigger, lo nibble rumble reduction */
    uint8_t  led[3];                        bool led_dirty;
    uint8_t  player_leds;                   bool player_dirty;
} DsOutput;

void ds_output_init(DsOutput *o, int fd, bool bluetooth);
void ds_output_set_rumble(DsOutput *o, uint8_t strong, uint8_t weak);
/* type/params as in ChiakiTriggerEffectsEvent (DualSense effect mode + 10 bytes). */
void ds_output_set_triggers(DsOutput *o, uint8_t type_l, const uint8_t *l, uint8_t type_r, const uint8_t *r);
void ds_output_triggers_off(DsOutput *o);
void ds_output_set_intensity(DsOutput *o, uint8_t intensity);
void ds_output_set_lightbar(DsOutput *o, uint8_t r, uint8_t g, uint8_t b);
void ds_output_set_player(DsOutput *o, uint8_t player_index);
/* Send one output report carrying everything that changed.  If the previous
 * report went out less than the minimum interval ago nothing is sent and the
 * changes stay pending for the next call.  Returns false if the write failed. */
bool ds_output_flush(DsOutput *o);
/* Like ds_output_flush but waits out the minimum interval first, so the report
 * is always sent.  For shutdown paths (motors/triggers off). */
bool ds_output_flush_sync(DsOutput *o);
