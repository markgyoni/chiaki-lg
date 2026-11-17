#include "pad_output.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#ifndef BUS_BLUETOOTH
#define BUS_BLUETOOTH 0x05
#endif

#define BITS_TEST(arr, bit) (((arr)[(bit) / 8]) & (1u << ((bit) % 8)))

/* Re-send a non-zero rumble this often: the Xbox report carries a duration
 * (max 2.55 s), so a long rumble would otherwise die out. */
#define KEEPALIVE_MS 1000

const char *pad_output_kind_name(PadOutputKind k)
{
    switch (k) {
    case PAD_OUT_EVDEV_FF:    return "evdev-ff";
    case PAD_OUT_XBOX_BT_HID: return "xbox-bt-hidraw";
    default:                  return "none";
    }
}

/* /dev/hidrawN belonging to the same HID device as /dev/input/eventM. */
int pad_output_open_sibling_hidraw(const char *event_path)
{
    const char *base = strrchr(event_path, '/');
    base = base ? base + 1 : event_path;

    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/input/%s/device/device/hidraw", base);
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    int fd = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/%s", ent->d_name);
        fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        break;
    }
    closedir(dir);
    return fd;
}

void pad_output_open(PadOutput *o, const char *event_path, int evdev_fd,
                     uint16_t vendor, uint16_t bustype)
{
    memset(o, 0, sizeof(*o));
    o->kind = PAD_OUT_NONE;
    o->evdev_fd = -1;
    o->hidraw_fd = -1;
    o->ff_id = -1;

    /* 1. Kernel force feedback, if the driver provides it. */
    uint8_t evbits[(EV_MAX + 7) / 8 + 1] = {0};
    uint8_t ffbits[(FF_MAX + 7) / 8 + 1] = {0};
    ioctl(evdev_fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
    if (BITS_TEST(evbits, EV_FF)) {
        ioctl(evdev_fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits);
        if (BITS_TEST(ffbits, FF_RUMBLE)) {
            int fl = fcntl(evdev_fd, F_GETFL);
            if ((fl & O_ACCMODE) == O_RDWR) {
                o->kind = PAD_OUT_EVDEV_FF;
                o->evdev_fd = evdev_fd;
                return;
            }
            snprintf(o->why_none, sizeof(o->why_none), "FF_RUMBLE but evdev not writable");
        }
    }

    /* 2. Raw HID for pads on hid-generic. */
    if (vendor == 0x045e && bustype == BUS_BLUETOOTH) {
        o->hidraw_fd = pad_output_open_sibling_hidraw(event_path);
        if (o->hidraw_fd >= 0) {
            o->kind = PAD_OUT_XBOX_BT_HID;
            return;
        }
        snprintf(o->why_none, sizeof(o->why_none), "hidraw open failed: %s", strerror(errno));
        return;
    }
    if (vendor == 0x054c) {
        /* Any output report switches a BT DualSense/DS4 to full input reports,
         * which hid-generic does not map to evdev — would kill the buttons.
         * Comes with the hidraw input backend (Stages 4 and 7). */
        snprintf(o->why_none, sizeof(o->why_none), "Sony pad: outputs go through the DualSense HID backend");
        return;
    }
    if (!o->why_none[0])
        snprintf(o->why_none, sizeof(o->why_none), "no force feedback");
}

static void evdev_ff_apply(PadOutput *o, uint8_t strong, uint8_t weak)
{
    if (strong == 0 && weak == 0) {
        if (o->ff_id >= 0) {
            struct input_event stop = { .type = EV_FF, .code = (uint16_t)o->ff_id, .value = 0 };
            (void)!write(o->evdev_fd, &stop, sizeof(stop));
        }
        return;
    }
    struct ff_effect eff;
    memset(&eff, 0, sizeof(eff));
    eff.type = FF_RUMBLE;
    eff.id = o->ff_id;                       /* -1 = new, else update in place */
    eff.u.rumble.strong_magnitude = (uint16_t)(strong * 257);
    eff.u.rumble.weak_magnitude   = (uint16_t)(weak * 257);
    eff.replay.length = 0;                   /* until stopped */
    if (ioctl(o->evdev_fd, EVIOCSFF, &eff) < 0)
        return;
    bool fresh = (o->ff_id < 0);
    o->ff_id = eff.id;
    if (fresh || (o->strong == 0 && o->weak == 0)) {
        struct input_event play = { .type = EV_FF, .code = (uint16_t)o->ff_id, .value = 1 };
        (void)!write(o->evdev_fd, &play, sizeof(play));
    }
}

/* Xbox One S / Series over Bluetooth, output report 0x03 (as in Linux
 * hid-microsoft xb1s_ff_report and SDL's HIDAPI Xbox driver):
 *   [0] 0x03  [1] enable mask (bit0 weak, bit1 strong, bit2/3 trigger motors)
 *   [2] left trigger  [3] right trigger  [4] strong (left)  [5] weak (right)  (0-100)
 *   [6] duration (10 ms units)  [7] start delay  [8] loop count */
static void xbox_bt_apply(PadOutput *o, uint8_t strong, uint8_t weak)
{
    uint8_t r[9] = { 0x03, 0x03, 0, 0, 0, 0, 0xff, 0x00, 0x00 };
    r[4] = (uint8_t)(strong * 100 / 255);
    r[5] = (uint8_t)(weak * 100 / 255);
    if (r[4] == 0 && r[5] == 0)
        r[6] = 0;
    (void)!write(o->hidraw_fd, r, sizeof(r));
}

void pad_output_rumble(PadOutput *o, uint8_t strong, uint8_t weak, long now_ms)
{
    if (o->kind == PAD_OUT_NONE) return;

    bool changed = (strong != o->strong || weak != o->weak);
    bool active  = (strong || weak);
    bool keepalive = active && o->kind == PAD_OUT_XBOX_BT_HID &&
                     now_ms - o->last_send_ms >= KEEPALIVE_MS;
    if (!changed && !keepalive) return;

    if (o->kind == PAD_OUT_EVDEV_FF) evdev_ff_apply(o, strong, weak);
    else                             xbox_bt_apply(o, strong, weak);

    o->strong = strong;
    o->weak = weak;
    o->last_send_ms = now_ms;
}

void pad_output_close(PadOutput *o)
{
    if (o->kind != PAD_OUT_NONE && (o->strong || o->weak))
        pad_output_rumble(o, 0, 0, o->last_send_ms + 1);
    if (o->kind == PAD_OUT_EVDEV_FF && o->ff_id >= 0)
        ioctl(o->evdev_fd, EVIOCRMFF, o->ff_id);
    if (o->hidraw_fd >= 0)
        close(o->hidraw_fd);
    o->hidraw_fd = -1;
    o->kind = PAD_OUT_NONE;
}
