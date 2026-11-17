#pragma once
/* Bluetooth output fix for the DualSense on LG webOS (see tools/btfix.c).
 *
 * LG's Bluetooth stack only sends HID output reports the way a DualSense
 * accepts them (DATA on the interrupt channel) for devices named "Xbox
 * Wireless Controller".  `btfix` patches that check in the running Bluetooth
 * service's memory so it matches any "Wireless Controller".  It needs root,
 * so the app runs it through the Homebrew Channel's exec service; on an
 * unrooted TV or an unknown firmware build the patch is unavailable and
 * DualSense outputs stay off over Bluetooth (USB is unaffected). */

#include <stdbool.h>

typedef enum {
    BT_PATCH_IDLE = 0,      /* not requested yet */
    BT_PATCH_RUNNING,       /* btfix is running in the background */
    BT_PATCH_ACTIVE,        /* patch applied (or already in place) */
    BT_PATCH_UNAVAILABLE,   /* not rooted / unknown build / error; see reason */
} BtPatchState;

/* Ask for the patch to be (re)applied.  Non-blocking: runs btfix on a worker
 * thread.  Safe to call often; a request while one is running is ignored, and
 * a permanent failure (unknown firmware build) is not retried. */
void bt_patch_request(void);

BtPatchState bt_patch_state(void);

/* Human-readable result of the last run (for the log). */
const char *bt_patch_reason(void);
