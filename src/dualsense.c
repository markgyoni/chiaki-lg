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
