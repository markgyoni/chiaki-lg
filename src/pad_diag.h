#pragma once
/* Controller diagnostics shared by the app (src/input.c) and the standalone
 * tools/paddiag.c CLI.  Everything here is read-only except
 * pad_diag_hidraw_sample() with request_feature=true (see below). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*pad_diag_log_fn)(const char *fmt, ...);

/* Human-readable names for evdev codes; returns NULL if unknown. */
const char *pad_diag_key_name(unsigned code);
const char *pad_diag_abs_name(unsigned code);

/* Kernel driver bound to the HID device behind /dev/input/eventN
 * (e.g. "hid-generic", "sony", "microsoft", "xpad").  Writes "" if none. */
void pad_diag_evdev_driver(const char *event_path, char *out, size_t out_sz);

/* Log uid/gid/supplementary groups of this process. */
void pad_diag_log_process_ids(pad_diag_log_fn log);

/* One block per /dev/input/event* node: name, bus/vid/pid, driver,
 * gamepad buttons, axes with ranges, force-feedback capabilities. */
void pad_diag_dump_evdev(pad_diag_log_fn log);

/* One line per /dev/hidraw* node: bus/vid/pid, name, descriptor size and
 * whether this process can open it read/write. */
void pad_diag_dump_hidraw(pad_diag_log_fn log);

/* Read input reports from a hidraw node for `ms` milliseconds and log a
 * histogram of (report id, length) plus a hex dump of the first report of
 * each kind.  If request_feature is true, first read Sony calibration
 * feature report 0x05.  NOTE: on a Bluetooth DualSense/DS4 that switches
 * the pad to full 0x31 reports, which hid-generic does not map to evdev,
 * so evdev buttons go dead until the controller reconnects. */
void pad_diag_hidraw_sample(pad_diag_log_fn log, const char *hidraw_path,
                            int ms, bool request_feature);

/* Thinning filter for noisy stick axes when logging raw ABS events:
 * returns true if `val` differs from `*last` by at least 1/16 of the
 * range or sits at an end stop; updates *last when it returns true. */
bool pad_diag_abs_significant(int val, int min, int max, int *last);
