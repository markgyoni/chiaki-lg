#include "dualsense.h"

#include <string.h>

#include <chiaki/controller.h>

bool ds_is_dualsense(uint16_t vendor, uint16_t product)
{
    return vendor == DS_VENDOR && (product == DS_PRODUCT || product == DS_EDGE_PRODUCT);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Hat switch value (0 = N, clockwise, 8 = released) → d-pad bits. */
static uint32_t hat_to_dpad(uint8_t hat)
{
    static const uint32_t U = CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    static const uint32_t D = CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    static const uint32_t L = CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    static const uint32_t R = CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    static const uint32_t map[8] = { U, U | R, R, D | R, D, D | L, L, U | L };
    return hat < 8 ? map[hat] : 0;
}

/* buttons[0..2] as in hid-playstation (DS_BUTTONS0_* ...). */
static uint32_t map_buttons(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint32_t out = hat_to_dpad(b0 & 0x0f);
    if (b0 & 0x10) out |= CHIAKI_CONTROLLER_BUTTON_BOX;
    if (b0 & 0x20) out |= CHIAKI_CONTROLLER_BUTTON_CROSS;
    if (b0 & 0x40) out |= CHIAKI_CONTROLLER_BUTTON_MOON;
    if (b0 & 0x80) out |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    if (b1 & 0x01) out |= CHIAKI_CONTROLLER_BUTTON_L1;
    if (b1 & 0x02) out |= CHIAKI_CONTROLLER_BUTTON_R1;
    /* b1 & 0x04 / 0x08: digital L2/R2 — analog values are used instead */
    if (b1 & 0x10) out |= CHIAKI_CONTROLLER_BUTTON_SHARE;     /* Create */
    if (b1 & 0x20) out |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    if (b1 & 0x40) out |= CHIAKI_CONTROLLER_BUTTON_L3;
    if (b1 & 0x80) out |= CHIAKI_CONTROLLER_BUTTON_R3;
    if (b2 & 0x01) out |= CHIAKI_CONTROLLER_BUTTON_PS;
    if (b2 & 0x02) out |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    return out;
}

static void parse_touch(const uint8_t *p, DsTouch *t)
{
    t->active = !(p[0] & 0x80);
    t->id = p[0] & 0x7f;
    t->x = (uint16_t)(p[1] | ((p[2] & 0x0f) << 8));
    t->y = (uint16_t)((p[2] >> 4) | (p[3] << 4));
}

/* Full payload (hid-playstation struct dualsense_input_report). */
static void parse_full(const uint8_t *d, DsInputState *o)
{
    memset(o, 0, sizeof(*o));
    o->full = true;
    o->lx = d[0]; o->ly = d[1];
    o->rx = d[2]; o->ry = d[3];
    o->l2 = d[4]; o->r2 = d[5];
    /* d[6] = sequence number */
    o->buttons = map_buttons(d[7], d[8], d[9]);
    o->mute = (d[9] & 0x04) != 0;
    for (int i = 0; i < 3; i++) {
        o->gyro[i]  = (int16_t)le16(d + 15 + 2 * i);
        o->accel[i] = (int16_t)le16(d + 21 + 2 * i);
    }
    o->sensor_timestamp = le32(d + 27);
    parse_touch(d + 32, &o->touch[0]);
    parse_touch(d + 36, &o->touch[1]);
}

bool ds_parse_input(const uint8_t *rep, size_t len, DsInputState *out)
{
    if (len >= 64 && rep[0] == 0x01) {          /* USB full */
        parse_full(rep + 1, out);
        return true;
    }
    if (len >= 78 && rep[0] == 0x31) {          /* BT full */
        parse_full(rep + 2, out);
        return true;
    }
    if (len >= 10 && rep[0] == 0x01) {          /* BT simple (DS4-style) */
        memset(out, 0, sizeof(*out));
        out->full = false;
        out->lx = rep[1]; out->ly = rep[2];
        out->rx = rep[3]; out->ry = rep[4];
        out->buttons = map_buttons(rep[5], rep[6], rep[7]);
        out->l2 = rep[8]; out->r2 = rep[9];
        return true;
    }
    return false;
}

/* ── Motion (Stage 6) ──────────────────────────────────────────────────────── */

#define DS_GYRO_RES_PER_DEG_S 1024   /* hid-playstation */
#define DS_ACC_RES_PER_G      8192
/* Nominal sensitivities when factory calibration is unavailable: ±2000 deg/s
 * full scale (16.384 LSB per deg/s) and 8192 LSB per g — matches the
 * calibrated scale of typical pads within a few percent. */
#define DS_NOMINAL_GYRO_LSB_PER_DEG_S 16.384f
#define DS_NOMINAL_ACC_LSB_PER_G      8192.0f

#define STILL_THRESHOLD   30     /* raw units (~1.8 deg/s) */
#define STILL_SAMPLES     200    /* ~1 s of BT reports */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool ds_parse_calibration(const uint8_t *buf, size_t len, DsCalib *c)
{
    memset(c, 0, sizeof(*c));
    if (len < 35 || buf[0] != 0x05)
        return false;
    const uint8_t *p = buf + 1;
#define S16(off) ((int16_t)le16(p + (off)))
    int pitch_bias = S16(0), yaw_bias = S16(2), roll_bias = S16(4);
    int pitch_plus = S16(6), pitch_minus = S16(8);
    int yaw_plus = S16(10), yaw_minus = S16(12);
    int roll_plus = S16(14), roll_minus = S16(16);
    int speed_plus = S16(18), speed_minus = S16(20);
    int acc_x_plus = S16(22), acc_x_minus = S16(24);
    int acc_y_plus = S16(26), acc_y_minus = S16(28);
    int acc_z_plus = S16(30), acc_z_minus = S16(32);
#undef S16
    int speed_2x = speed_plus + speed_minus;
    int gb[3] = { pitch_bias, yaw_bias, roll_bias };
    int gr[3] = { pitch_plus - pitch_minus, yaw_plus - yaw_minus, roll_plus - roll_minus };
    int ap[3] = { acc_x_plus, acc_y_plus, acc_z_plus };
    int am[3] = { acc_x_minus, acc_y_minus, acc_z_minus };
    for (int i = 0; i < 3; i++) {
        c->gyro_bias[i]  = gb[i];
        c->gyro_numer[i] = speed_2x * DS_GYRO_RES_PER_DEG_S;
        c->gyro_denom[i] = gr[i];
        int range_2g = ap[i] - am[i];
        c->acc_bias[i]  = ap[i] - range_2g / 2;
        c->acc_numer[i] = 2 * DS_ACC_RES_PER_G;
        c->acc_denom[i] = range_2g;
        if (c->gyro_denom[i] == 0 || c->acc_denom[i] == 0)
            return false;   /* bogus data: keep nominal scaling */
    }
    c->valid = true;
    return true;
}

void ds_motion_init(DsMotion *m, const DsCalib *cal)
{
    memset(m, 0, sizeof(*m));
    if (cal) m->cal = *cal;
    chiaki_orientation_tracker_init(&m->tracker);
    chiaki_accel_new_zero_set_inactive(&m->accel_zero, false);
}

static void track_still(DsMotion *m, const int16_t g[3])
{
    bool still = true;
    for (int i = 0; i < 3; i++) {
        int d = g[i] - m->still_ref[i];
        if (d < -STILL_THRESHOLD || d > STILL_THRESHOLD) { still = false; break; }
    }
    if (!still || m->still_count == 0) {
        for (int i = 0; i < 3; i++) { m->still_ref[i] = g[i]; m->still_sum[i] = 0; }
        m->still_count = 0;
        if (!still) return;
    }
    for (int i = 0; i < 3; i++) m->still_sum[i] += g[i];
    if (++m->still_count >= STILL_SAMPLES) {
        for (int i = 0; i < 3; i++) m->auto_bias[i] = (float)m->still_sum[i] / m->still_count;
        m->auto_bias_valid = true;
        m->still_count = 0;
    }
}

void ds_motion_update(DsMotion *m, const DsInputState *s, ChiakiControllerState *st)
{
    if (!s->full) return;

    float gyro[3], acc[3];   /* rad/s, g */
    if (m->cal.valid) {
        for (int i = 0; i < 3; i++) {
            float deg = (float)(s->gyro[i] - m->cal.gyro_bias[i]) * m->cal.gyro_numer[i]
                        / m->cal.gyro_denom[i] / DS_GYRO_RES_PER_DEG_S;
            gyro[i] = deg * (float)M_PI / 180.0f;
            acc[i]  = (float)(s->accel[i] - m->cal.acc_bias[i]) * m->cal.acc_numer[i]
                      / m->cal.acc_denom[i] / DS_ACC_RES_PER_G;
        }
    } else {
        track_still(m, s->gyro);
        for (int i = 0; i < 3; i++) {
            float raw = (float)s->gyro[i] - (m->auto_bias_valid ? m->auto_bias[i] : 0.0f);
            gyro[i] = raw / DS_NOMINAL_GYRO_LSB_PER_DEG_S * (float)M_PI / 180.0f;
            acc[i]  = (float)s->accel[i] / DS_NOMINAL_ACC_LSB_PER_G;
        }
    }

    /* Sensor timestamp counts in units of 1/3 us and wraps at 32 bits. */
    if (m->have_ts) {
        uint32_t delta = s->sensor_timestamp - m->last_ts;
        m->ts_us += delta / 3;
    }
    m->last_ts = s->sensor_timestamp;
    m->have_ts = true;

    for (int i = 0; i < 3; i++) m->last_accel[i] = acc[i];

    chiaki_orientation_tracker_update(&m->tracker, gyro[0], gyro[1], gyro[2],
                                      acc[0], acc[1], acc[2], &m->accel_zero, false,
                                      (uint32_t)m->ts_us);
    chiaki_orientation_tracker_apply_to_controller_state(&m->tracker, st);
}

void ds_motion_reset(DsMotion *m)
{
    /* As chiaki-ng: take the current pose as the new zero. */
    chiaki_accel_new_zero_set_active(&m->accel_zero, m->last_accel[0], m->last_accel[1],
                                     m->last_accel[2], false);
    chiaki_orientation_tracker_init(&m->tracker);
}
