#pragma once
/* Controller outputs (rumble for now).  Picks a transport per pad:
 *   - evdev force feedback (FF_RUMBLE) when the kernel driver supports it
 *   - raw HID output reports for pads whose driver does not (webOS runs
 *     most Bluetooth pads on hid-generic, which has no force feedback)
 * See ROADMAP.md Stage 3. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAD_OUT_NONE = 0,
    PAD_OUT_EVDEV_FF,     /* EVIOCSFF + EV_FF writes on the evdev node */
    PAD_OUT_XBOX_BT_HID,  /* Xbox One S / Series over BT: hidraw report 0x03 */
} PadOutputKind;

typedef struct {
    PadOutputKind kind;
    int      evdev_fd;      /* borrowed from the input side (must be O_RDWR) */
    int      hidraw_fd;     /* owned */
    int16_t  ff_id;         /* uploaded effect id, -1 if none */
    uint8_t  strong, weak;  /* last applied values (0-255) */
    long     last_send_ms;
    char     why_none[64];  /* reason when kind == PAD_OUT_NONE, for the log */
} PadOutput;

/* event_path: /dev/input/eventN of the pad; evdev_fd opened O_RDWR if possible. */
void pad_output_open(PadOutput *o, const char *event_path, int evdev_fd,
                     uint16_t vendor, uint16_t bustype);
void pad_output_close(PadOutput *o);   /* stops rumble first */

/* strong = low-frequency (left) motor, weak = high-frequency (right) motor.
 * Cheap to call repeatedly; only sends on change or for keep-alive. */
void pad_output_rumble(PadOutput *o, uint8_t strong, uint8_t weak, long now_ms);

const char *pad_output_kind_name(PadOutputKind k);
