#include "input.h"
#include "app_log.h"
#include "pad_diag.h"
#include "pad_profile.h"
#include "pad_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>

#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <linux/input.h>

// ── Fallback defines for old kernel headers (pre-3.16 webOS sysroot) ──────────
// These gamepad layout aliases were added in Linux 3.16.
// The webOS toolchain sysroot ships with older headers that only have BTN_A/B/X/Y.
#ifndef BTN_SOUTH
#define BTN_SOUTH    0x130
#endif

#include <chiaki/controller.h>

// ── webOS magic key codes (TV remote via SDL_KEYDOWN) ─────────────────────────
#define WEBOS_KEY_UP        1073741906
#define WEBOS_KEY_DOWN      1073741905
#define WEBOS_KEY_LEFT      1073741904
#define WEBOS_KEY_RIGHT     1073741903
#define WEBOS_KEY_OK        13
#define WEBOS_KEY_BACK      1073742094
#define WEBOS_KEY_RED       403
#define WEBOS_KEY_GREEN     404
#define WEBOS_KEY_YELLOW    405
#define WEBOS_KEY_BLUE      406

// ── Evdev bit-test helper ──────────────────────────────────────────────────────
#define EVDEV_BITS_TEST(arr, bit) \
    (((arr)[(bit) / 8]) & (1 << ((bit) % 8)))

#define MAX_GAMEPADS 4

// Hotplug: /dev/input is watched with inotify; after a change we wait a bit
// before rescanning because udev fixes up node permissions after creation.
#define RESCAN_DELAY_MS     500
#define RESCAN_FALLBACK_MS  3000   // periodic rescan if inotify is unavailable

// ── Touchpad chord: Select+Start → Touchpad (pads without a touchpad) ────────
// When Select or Start is pressed alone, we defer emitting SHARE/OPTIONS for
// up to CHORD_WINDOW_MS to see if the other button follows.  If both arrive
// within the window → emit TOUCHPAD instead.
#define CHORD_WINDOW_MS 100

// Haptics → rumble conversion constants (from chiaki-ng gui/src/streamsession.cpp)
#define HAPTIC_RUMBLE_MIN_STRENGTH 100
#define HAPTIC_FRAMES_PER_TICK     3
#define HAPTIC_TICK_MS             (HAPTIC_FRAMES_PER_TICK * 10)

typedef struct {
    bool            select_held;    // physical Select is currently down
    bool            start_held;     // physical Start is currently down
    bool            chord_active;   // chord detected → TOUCHPAD is pressed
    uint16_t        pending_btn;    // BTN_SELECT or BTN_START (0 = none)
    struct timespec pending_ts;     // when the pending button was pressed
    uint32_t        tap_pending;    // chiaki button bit needing immediate release after tap
} ChordState;

typedef struct {
    int  fd;                    // -1 = free slot
    char path[64];
    char name[128];
    int  abs_min[ABS_CNT];
    int  abs_max[ABS_CNT];
    int  abs_logged[ABS_CNT];   // last value logged in input_debug mode
    ChordState chord;
    PadProfile profile;         // evdev code → chiaki mapping for this device
    ChiakiControllerState state;  // this pad's contribution
    PadOutput  out;             // rumble transport for this pad
    bool dirty;                 // state changed since the last SYN_REPORT
    bool dropped;               // SYN_DROPPED seen: ignore events until next SYN_REPORT, then resync
} GamepadDev;

struct InputContext {
    pthread_mutex_t       state_mutex;   // guards everything below + session sends
    ChiakiControllerState remote_state;  // TV remote (SDL keyboard events)
    ChiakiControllerState last_sent;
    bool                  have_last_sent;

    GamepadDev    pads[MAX_GAMEPADS];

    pthread_t     reader_thread;
    volatile bool running;
    bool          debug;
    char          profile_override[32];
    bool          rumble_enabled;
    uint8_t       rumble_strong, rumble_weak;   // latest PS5 rumble request

    // Haptics → rumble (PS5 DualSense mode).  One strength value per haptics
    // frame is queued by the audio thread; the input thread averages
    // HAPTIC_FRAMES_PER_TICK of them every HAPTIC_TICK_MS (chiaki-ng's scheme).
    uint16_t        haptic_queue[32];
    int             haptic_head, haptic_count;
    uint8_t         haptic_level;               // current motor level from haptics
    struct timespec haptic_tick;
    unsigned long   haptic_frames;              // stats for the log
    unsigned long   rumble_events;

    int             inotify_fd;          // -1 if unavailable
    bool            rescan_pending;
    struct timespec rescan_at;
    struct timespec last_scan;

    ChiakiSession *session;
};

// ── Normalize evdev ABS value → int16 ─────────────────────────────────────────
static int16_t normalize_axis(int val, int min, int max)
{
    if (max == min) return 0;
    int32_t scaled = (int32_t)(((int64_t)(val - min) * 65534) / (max - min)) - 32767;
    if (scaled >  32767) scaled =  32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static uint8_t normalize_trigger(int val, int min, int max)
{
    if (max <= min) return 0;
    if (val <= min) return 0;
    if (val >= max) return 255;
    return (uint8_t)(((int64_t)(val - min) * 255) / (max - min));
}

// ── Monotonic clock helpers ────────────────────────────────────────────────────
static void get_monotonic(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long ms_elapsed(const struct timespec *from)
{
    struct timespec now;
    get_monotonic(&now);
    return (now.tv_sec - from->tv_sec) * 1000L +
           (now.tv_nsec - from->tv_nsec) / 1000000L;
}

// ── Send the combined state of all pads + TV remote to the session ────────────
// Caller holds state_mutex.  Sending under the mutex means that once
// input_set_session(ctx, NULL) returns, no send to the old session is in flight.
static void publish_locked(InputContext *ctx, ChiakiSession *session, bool force)
{
    ChiakiControllerState out = ctx->remote_state;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (ctx->pads[i].fd < 0) continue;
        ChiakiControllerState merged;
        chiaki_controller_state_or(&merged, &out, &ctx->pads[i].state);
        out = merged;
    }

    if (!force && ctx->have_last_sent && chiaki_controller_state_equals(&out, &ctx->last_sent))
        return;
    ctx->last_sent = out;
    ctx->have_last_sent = true;

    if (session)
        chiaki_session_set_controller_state(session, &out);
}

// ── Flush a deferred chord-pending button as its normal mapping ───────────────
static void chord_flush_pending(ChiakiControllerState *st, ChordState *ch)
{
    if (ch->pending_btn == BTN_SELECT) {
        st->buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
        app_log("[INPUT] Chord timeout — flushing Select as Share\n");
    } else if (ch->pending_btn == BTN_START) {
        st->buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
        app_log("[INPUT] Chord timeout — flushing Start as Options\n");
    }
    ch->pending_btn = 0;
}

// ── Map evdev EV_KEY → chiaki button (with Select+Start chord detection) ─────
// Returns true if state was modified, false otherwise.
static bool apply_key_event(ChiakiControllerState *st, GamepadDev *pad,
                            uint16_t code, int32_t val)
{
    bool pressed = (val != 0);
    ChordState *ch = &pad->chord;

    if (val == 2) return false;  // autorepeat

    // ── Select / Start: chord detection for Touchpad (pads without one) ──────
    if (pad->profile.chord_touchpad && (code == BTN_SELECT || code == BTN_START)) {
        bool is_select = (code == BTN_SELECT);

        if (pressed) {
            if (is_select) ch->select_held = true;
            else           ch->start_held  = true;

            // Check if the other button is already pending → chord!
            bool other_pending = (is_select && ch->pending_btn == BTN_START) ||
                                 (!is_select && ch->pending_btn == BTN_SELECT);
            bool other_held    = is_select ? ch->start_held : ch->select_held;

            if (other_pending || (other_held && ch->chord_active)) {
                // Chord detected — clear any individual presses, set touchpad
                st->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_SHARE |
                                 CHIAKI_CONTROLLER_BUTTON_OPTIONS);
                st->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
                ch->chord_active = true;
                ch->pending_btn  = 0;
                app_log("[INPUT] Chord detected: Select+Start → Touchpad\n");
                return true;
            }

            // No chord yet — defer this button
            ch->pending_btn = code;
            get_monotonic(&ch->pending_ts);
            return false;  // don't send state yet; wait for chord window
        }
        else {
            // Release
            if (is_select) ch->select_held = false;
            else           ch->start_held  = false;

            if (ch->chord_active) {
                // Releasing one of the chord buttons → clear touchpad
                if (!ch->select_held && !ch->start_held) {
                    st->buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
                    ch->chord_active = false;
                    app_log("[INPUT] Chord released: Touchpad cleared\n");
                }
                return true;
            }

            // If this button was still pending (quick tap+release before window),
            // flush it as a press; caller will send an immediate release via tap_pending.
            if (ch->pending_btn == code) {
                uint32_t btn_bit = is_select ? CHIAKI_CONTROLLER_BUTTON_SHARE
                                             : CHIAKI_CONTROLLER_BUTTON_OPTIONS;
                st->buttons |= btn_bit;
                ch->pending_btn  = 0;
                ch->tap_pending  = btn_bit;  // signal immediate release needed
                return true;
            }

            // Normal release (was already flushed as individual button)
            if (is_select) st->buttons &= ~CHIAKI_CONTROLLER_BUTTON_SHARE;
            else           st->buttons &= ~CHIAKI_CONTROLLER_BUTTON_OPTIONS;
            return true;
        }
    }

    // ── All other buttons: profile table ─────────────────────────────────────
    uint32_t btn = pad_profile_button(&pad->profile, code);
    if (!btn) return false;

    if (btn == PAD_BTN_L2_DIGITAL) { st->l2_state = pressed ? 255 : 0; return true; }
    if (btn == PAD_BTN_R2_DIGITAL) { st->r2_state = pressed ? 255 : 0; return true; }

    if (pressed) st->buttons |= btn;
    else         st->buttons &= ~btn;
    return true;
}

// ── Map evdev EV_ABS → chiaki sticks/triggers/dpad ────────────────────────────
static bool apply_abs_event(ChiakiControllerState *st, GamepadDev *pad,
                            uint16_t code, int32_t val)
{
    if (code >= ABS_CNT) return false;
    int min = pad->abs_min[code];
    int max = pad->abs_max[code];

    // Which evdev axis is which stick/trigger depends on the driver; the
    // per-controller profile (src/pad_profile.c) says.
    switch (pad_profile_axis(&pad->profile, code)) {
    case PAD_AXIS_LX: st->left_x  = normalize_axis(val, min, max); return true;
    case PAD_AXIS_LY: st->left_y  = normalize_axis(val, min, max); return true;
    case PAD_AXIS_RX: st->right_x = normalize_axis(val, min, max); return true;
    case PAD_AXIS_RY: st->right_y = normalize_axis(val, min, max); return true;
    case PAD_AXIS_L2: st->l2_state = normalize_trigger(val, min, max); return true;
    case PAD_AXIS_R2: st->r2_state = normalize_trigger(val, min, max); return true;
    case PAD_AXIS_HATX:
        st->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT |
                         CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT);
        if (val < 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
        if (val > 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
        return true;
    case PAD_AXIS_HATY:
        st->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_DPAD_UP |
                         CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN);
        if (val < 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
        if (val > 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
        return true;
    default:
        return false;
    }
}

// ── Rebuild a pad's state from the kernel's current view ───────────────────────
// Used after SYN_DROPPED (event queue overflow) and when a pad is first opened,
// so a button held at that moment is not lost.  Caller holds state_mutex.
static void resync_pad_locked(GamepadDev *pad)
{
    uint8_t keys[(KEY_MAX + 7) / 8 + 1];
    memset(keys, 0, sizeof(keys));
    ioctl(pad->fd, EVIOCGKEY(sizeof(keys)), keys);

    chiaki_controller_state_set_idle(&pad->state);
    memset(&pad->chord, 0, sizeof(pad->chord));

    for (unsigned code = 0x100; code < 0x2ff; code++)
        if (EVDEV_BITS_TEST(keys, code))
            apply_key_event(&pad->state, pad, (uint16_t)code, 1);
    // No chord deferral on resync: a held Select/Start counts as pressed.
    if (pad->chord.pending_btn)
        chord_flush_pending(&pad->state, &pad->chord);
    pad->chord.tap_pending = 0;

    for (unsigned axis = 0; axis < ABS_CNT; axis++) {
        if (pad_profile_axis(&pad->profile, (uint16_t)axis) == PAD_AXIS_NONE) continue;
        struct input_absinfo ai;
        if (ioctl(pad->fd, EVIOCGABS(axis), &ai) == 0)
            apply_abs_event(&pad->state, pad, (uint16_t)axis, ai.value);
    }
}

// ── Check if evdev node is a physical gamepad ──────────────────────────────────
static bool is_gamepad(int fd, const char *name)
{
    // Skip the virtual LGE/LG mirrored devices that webOS hidd creates.
    if (strncmp(name, "LGE", 3) == 0 || strncmp(name, "LG ", 3) == 0)
        return false;

    uint8_t keybits[(KEY_MAX + 7) / 8];
    memset(keybits, 0, sizeof(keybits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

    // Must have BTN_SOUTH (A/Cross) AND analog sticks (ABS_X+ABS_Y) to be a gamepad.
    // Checking BTN_SOUTH alone lets through TV peripheral devices (IoT keypads,
    // Bluetooth audio sources, etc.) that happen to have 0x130 set.
    if (!EVDEV_BITS_TEST(keybits, BTN_SOUTH))
        return false;

    uint8_t absbits[(ABS_MAX + 7) / 8];
    memset(absbits, 0, sizeof(absbits));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

    return EVDEV_BITS_TEST(absbits, ABS_X) && EVDEV_BITS_TEST(absbits, ABS_Y);
}

static bool pad_path_open(InputContext *ctx, const char *path)
{
    for (int i = 0; i < MAX_GAMEPADS; i++)
        if (ctx->pads[i].fd >= 0 && strcmp(ctx->pads[i].path, path) == 0)
            return true;
    return false;
}

// ── Open, classify and exclusively grab one evdev node if it is a gamepad ─────
// Caller holds state_mutex.
static void try_open_pad_locked(InputContext *ctx, const char *path)
{
    if (pad_path_open(ctx, path)) return;

    int slot = -1;
    for (int i = 0; i < MAX_GAMEPADS; i++)
        if (ctx->pads[i].fd < 0) { slot = i; break; }
    if (slot < 0) return;

    // Read/write so force feedback can be written where the driver supports it.
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;

    char devname[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(devname) - 1), devname);

    if (!is_gamepad(fd, devname)) { close(fd); return; }

    // Real controllers sit behind a kernel HID/USB driver.  LG's virtual
    // remote/IME nodes ("Smart Remote RCU Input", "LGE Network Input")
    // have none, yet advertise every key and axis.
    char drv[64];
    pad_diag_evdev_driver(path, drv, sizeof(drv));
    if (!drv[0]) { close(fd); return; }

    GamepadDev *pad = &ctx->pads[slot];
    memset(pad, 0, sizeof(*pad));
    pad->fd = -1;

    // Read axis ranges for proper normalization
    int abs_value[ABS_CNT];
    memset(abs_value, 0, sizeof(abs_value));
    for (int axis = 0; axis < ABS_CNT; axis++) {
        struct input_absinfo info;
        if (ioctl(fd, EVIOCGABS(axis), &info) == 0) {
            pad->abs_min[axis] = info.minimum;
            pad->abs_max[axis] = info.maximum;
            pad->abs_logged[axis] = info.value;
            abs_value[axis] = info.value;
        }
    }

    struct input_id id;
    memset(&id, 0, sizeof(id));
    ioctl(fd, EVIOCGID, &id);

    uint8_t keybits[(KEY_MAX + 7) / 8 + 1];
    uint8_t absbits[(ABS_MAX + 7) / 8 + 1];
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

    PadDeviceInfo info = {
        .bustype = id.bustype, .vendor = id.vendor, .product = id.product,
        .driver = drv, .keybits = keybits, .absbits = absbits,
        .abs_min = pad->abs_min, .abs_max = pad->abs_max, .abs_value = abs_value,
    };
    pad->profile = pad_profile_select(&info, ctx->profile_override);

    // EVIOCGRAB: take exclusive ownership for the whole app lifetime (also
    // between reconnect attempts), so webOS's hidd/compositor never see this
    // pad — no B→Back, A→OK, Guide→Home interceptions.  SDL's own fd for the
    // device stops receiving events too; we route everything ourselves.
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        app_log_always("[INPUT] EVIOCGRAB FAILED for \"%s\" %s: %s\n",
                       devname, path, strerror(errno));
        app_log_always("[INPUT] System button intercept (B=Back, A=OK) may still occur\n");
    }

    snprintf(pad->path, sizeof(pad->path), "%s", path);
    snprintf(pad->name, sizeof(pad->name), "%s", devname);
    pad->fd = fd;
    resync_pad_locked(pad);
    pad_output_open(&pad->out, path, fd, id.vendor, id.bustype);

    app_log_always("[INPUT] Opened gamepad: \"%s\" @ %s bus=0x%02x vid=%04x pid=%04x driver=%s "
                   "profile=%s (%s)%s\n",
                   devname, path, id.bustype, id.vendor, id.product, drv,
                   pad->profile.id, pad->profile.description,
                   pad->profile.chord_touchpad ? " touchpad=Select+Start chord" : "");
    app_log_always("[INPUT]   rumble: %s%s%s%s\n", pad_output_kind_name(pad->out.kind),
                   pad->out.why_none[0] ? " (" : "", pad->out.why_none,
                   pad->out.why_none[0] ? ")" : "");
}

// Caller holds state_mutex.
static void close_pad_locked(InputContext *ctx, int i, const char *why)
{
    GamepadDev *pad = &ctx->pads[i];
    if (pad->fd < 0) return;
    app_log_always("[INPUT] Gamepad \"%s\" (%s) removed: %s\n", pad->name, pad->path, why);
    pad_output_close(&pad->out);
    ioctl(pad->fd, EVIOCGRAB, 0);
    close(pad->fd);
    pad->fd = -1;
    // Its held buttons/sticks must not stay pressed on the PS5.
    chiaki_controller_state_set_idle(&pad->state);
    publish_locked(ctx, ctx->session, false);
}

// Scan /dev/input for gamepads not opened yet.  Caller holds state_mutex.
static void scan_pads_locked(InputContext *ctx)
{
    get_monotonic(&ctx->last_scan);
    ctx->rescan_pending = false;

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        app_log("[INPUT] opendir(/dev/input) failed: %s\n", strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        try_open_pad_locked(ctx, path);
    }
    closedir(dir);

    // A pad that just appeared may already have buttons held.
    publish_locked(ctx, ctx->session, false);
}

static int count_pads(InputContext *ctx)
{
    int n = 0;
    for (int i = 0; i < MAX_GAMEPADS; i++)
        if (ctx->pads[i].fd >= 0) n++;
    return n;
}

// ── Handle one batch of events from a pad ──────────────────────────────────────
// Caller holds state_mutex.
static void handle_pad_events_locked(InputContext *ctx, int i)
{
    GamepadDev *pad = &ctx->pads[i];
    struct input_event evs[64];

    for (;;) {
        ssize_t n = read(pad->fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            close_pad_locked(ctx, i, strerror(errno));   // ENODEV on unplug / BT drop
            return;
        }
        if (n == 0) return;

        int count = (int)(n / (ssize_t)sizeof(evs[0]));
        for (int k = 0; k < count; k++) {
            struct input_event *ev = &evs[k];

            if (ev->type == EV_SYN) {
                if (ev->code == SYN_DROPPED) {
                    pad->dropped = true;
                } else if (ev->code == SYN_REPORT) {
                    if (pad->dropped) {
                        pad->dropped = false;
                        app_log("[INPUT] %s: SYN_DROPPED, resyncing\n", pad->path);
                        resync_pad_locked(pad);
                        pad->dirty = true;
                    }
                    if (pad->dirty) {
                        pad->dirty = false;
                        publish_locked(ctx, ctx->session, false);
                    }
                    // Quick Select/Start tap on chord pads: press went out above,
                    // release now so the PS5 sees a full tap.
                    if (pad->chord.tap_pending) {
                        pad->state.buttons &= ~pad->chord.tap_pending;
                        pad->chord.tap_pending = 0;
                        publish_locked(ctx, ctx->session, false);
                    }
                }
                continue;
            }
            if (pad->dropped) continue;

            if (ctx->debug) {
                const char *dev = strrchr(pad->path, '/') + 1;
                if (ev->type == EV_KEY) {
                    const char *nm = pad_diag_key_name(ev->code);
                    app_log_always("[PADEV] %s KEY 0x%03x %s %d\n", dev, ev->code,
                                   nm ? nm : "?", ev->value);
                } else if (ev->type == EV_ABS && ev->code < ABS_CNT &&
                           pad_diag_abs_significant(ev->value, pad->abs_min[ev->code],
                                                    pad->abs_max[ev->code],
                                                    &pad->abs_logged[ev->code])) {
                    const char *nm = pad_diag_abs_name(ev->code);
                    app_log_always("[PADEV] %s ABS 0x%02x %s %d\n", dev, ev->code,
                                   nm ? nm : "?", ev->value);
                }
            }

            if (ev->type == EV_KEY)
                pad->dirty |= apply_key_event(&pad->state, pad, ev->code, ev->value);
            else if (ev->type == EV_ABS)
                pad->dirty |= apply_abs_event(&pad->state, pad, ev->code, ev->value);
        }
    }
}

// ── Evdev reader thread ────────────────────────────────────────────────────────
static void *evdev_reader(void *arg)
{
    InputContext *ctx = (InputContext *)arg;

    while (ctx->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;

        pthread_mutex_lock(&ctx->state_mutex);
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (ctx->pads[i].fd >= 0) {
                FD_SET(ctx->pads[i].fd, &rfds);
                if (ctx->pads[i].fd > maxfd) maxfd = ctx->pads[i].fd;
            }
        }
        pthread_mutex_unlock(&ctx->state_mutex);
        if (ctx->inotify_fd >= 0) {
            FD_SET(ctx->inotify_fd, &rfds);
            if (ctx->inotify_fd > maxfd) maxfd = ctx->inotify_fd;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 }; // 20ms for chord responsiveness
        int sel = (maxfd >= 0) ? select(maxfd + 1, &rfds, NULL, NULL, &tv)
                               : (usleep(20000), 0);

        pthread_mutex_lock(&ctx->state_mutex);

        // ── Hotplug ──────────────────────────────────────────────────────────
        if (sel > 0 && ctx->inotify_fd >= 0 && FD_ISSET(ctx->inotify_fd, &rfds)) {
            char buf[4096];
            while (read(ctx->inotify_fd, buf, sizeof(buf)) > 0) { /* drain */ }
            ctx->rescan_pending = true;
            get_monotonic(&ctx->rescan_at);
        }
        if ((ctx->rescan_pending && ms_elapsed(&ctx->rescan_at) >= RESCAN_DELAY_MS) ||
            (ctx->inotify_fd < 0 && ms_elapsed(&ctx->last_scan) >= RESCAN_FALLBACK_MS))
            scan_pads_locked(ctx);

        // ── Chord timeouts (checked every iteration, even without events) ────
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            GamepadDev *pad = &ctx->pads[i];
            if (pad->fd < 0) continue;
            if (pad->chord.pending_btn && ms_elapsed(&pad->chord.pending_ts) >= CHORD_WINDOW_MS) {
                chord_flush_pending(&pad->state, &pad->chord);
                publish_locked(ctx, ctx->session, false);
            }
        }

        // ── Pad events ───────────────────────────────────────────────────────
        if (sel > 0) {
            for (int i = 0; i < MAX_GAMEPADS; i++) {
                int fd = ctx->pads[i].fd;
                if (fd >= 0 && FD_ISSET(fd, &rfds))
                    handle_pad_events_locked(ctx, i);
            }
        }

        // ── Rumble (applied here so the chiaki callback never blocks on I/O) ─
        {
            if (ms_elapsed(&ctx->haptic_tick) >= HAPTIC_TICK_MS) {
                get_monotonic(&ctx->haptic_tick);
                uint32_t sum = 0;
                for (int k = 0; k < HAPTIC_FRAMES_PER_TICK && ctx->haptic_count > 0; k++) {
                    sum += ctx->haptic_queue[ctx->haptic_head];
                    ctx->haptic_head = (ctx->haptic_head + 1) % 32;
                    ctx->haptic_count--;
                }
                uint32_t strength = sum / HAPTIC_FRAMES_PER_TICK;   // 0..65535
                uint8_t level = (uint8_t)(strength >> 8);
                if (strength > 0 && level == 0) level = 1;
                if (ctx->debug && level != ctx->haptic_level)
                    app_log_always("[HAPTICS] level=%u (frames so far %lu)\n",
                                   level, ctx->haptic_frames);
                ctx->haptic_level = level;
            }

            struct timespec now;
            get_monotonic(&now);
            long now_ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
            uint8_t s = ctx->rumble_strong > ctx->haptic_level ? ctx->rumble_strong : ctx->haptic_level;
            uint8_t w = ctx->rumble_weak   > ctx->haptic_level ? ctx->rumble_weak   : ctx->haptic_level;
            if (!ctx->rumble_enabled) s = w = 0;
            for (int i = 0; i < MAX_GAMEPADS; i++)
                if (ctx->pads[i].fd >= 0)
                    pad_output_rumble(&ctx->pads[i].out, s, w, now_ms);
        }

        pthread_mutex_unlock(&ctx->state_mutex);
    }

    return NULL;
}

// ── Public API ─────────────────────────────────────────────────────────────────

// Capability dump of every input/hidraw node, logged once per launch so
// controller reports from users come with everything needed to map them.
static void log_controller_diagnostics(bool debug)
{
    pad_diag_log_process_ids(app_log_always);
    pad_diag_dump_evdev(app_log_always);
    pad_diag_dump_hidraw(app_log_always);

    if (!debug) return;
    // Passive sample of raw reports (no feature request, so evdev keeps working).
    DIR *dir = opendir("/dev");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
        pad_diag_hidraw_sample(app_log_always, path, 500, false);
    }
    closedir(dir);
}

InputContext *input_init(const InputOptions *opt)
{
    InputContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    chiaki_controller_state_set_idle(&ctx->remote_state);
    pthread_mutex_init(&ctx->state_mutex, NULL);
    ctx->running = true;
    ctx->session = NULL;
    ctx->debug   = opt->debug;
    ctx->rumble_enabled = opt->rumble;
    for (int i = 0; i < MAX_GAMEPADS; i++) ctx->pads[i].fd = -1;
    snprintf(ctx->profile_override, sizeof(ctx->profile_override), "%s",
             (opt->profile_override && opt->profile_override[0]) ? opt->profile_override : "auto");
    app_log_always("[INPUT] controller_profile=%s (choices: %s) rumble=%s\n",
                   ctx->profile_override, pad_profile_ids(), opt->rumble ? "on" : "off");

    log_controller_diagnostics(opt->debug);

    ctx->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ctx->inotify_fd >= 0 &&
        inotify_add_watch(ctx->inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(ctx->inotify_fd);
        ctx->inotify_fd = -1;
    }
    if (ctx->inotify_fd < 0)
        app_log_always("[INPUT] inotify unavailable (%s) — rescanning every %d ms\n",
                       strerror(errno), RESCAN_FALLBACK_MS);

    pthread_mutex_lock(&ctx->state_mutex);
    scan_pads_locked(ctx);
    int n = count_pads(ctx);
    pthread_mutex_unlock(&ctx->state_mutex);
    if (n == 0)
        app_log_always("[INPUT] No gamepads yet — TV remote only until one connects\n");

    pthread_create(&ctx->reader_thread, NULL, evdev_reader, ctx);
    return ctx;
}

void input_set_session(InputContext *ctx, ChiakiSession *session)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->session = session;
    // New session starts from the current physical state (e.g. a button held
    // across a reconnect).
    if (session)
        publish_locked(ctx, session, true);
    if (!session)
        app_log_always("[RUMBLE] session summary: rumble events=%lu haptics frames=%lu\n",
                       ctx->rumble_events, ctx->haptic_frames);
    ctx->rumble_events = ctx->haptic_frames = 0;
    // No session → nothing may keep a motor running.
    ctx->rumble_strong = ctx->rumble_weak = 0;
    ctx->haptic_count = 0;
    ctx->haptic_level = 0;
    pthread_mutex_unlock(&ctx->state_mutex);
}

void input_set_rumble(InputContext *ctx, uint8_t strong, uint8_t weak)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->state_mutex);
    if (ctx->debug && (strong != ctx->rumble_strong || weak != ctx->rumble_weak))
        app_log_always("[RUMBLE] strong=%u weak=%u\n", strong, weak);
    ctx->rumble_events++;
    ctx->rumble_strong = strong;
    ctx->rumble_weak   = weak;
    pthread_mutex_unlock(&ctx->state_mutex);
}

void input_push_haptics(InputContext *ctx, const uint8_t *buf, size_t buf_size)
{
    if (!ctx || buf_size == 0 || (buf_size % 4) != 0) return;

    // Mean absolute amplitude per channel (x2), as chiaki-ng does.
    size_t n = buf_size / 4;
    uint32_t suml = 0, sumr = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t l, r;
        memcpy(&l, buf + i * 4, 2);
        memcpy(&r, buf + i * 4 + 2, 2);
        suml += (uint32_t)(l < 0 ? -(int32_t)l : l) * 2;
        sumr += (uint32_t)(r < 0 ? -(int32_t)r : r) * 2;
    }
    uint32_t left  = suml / n, right = sumr / n;
    if (left  <= HAPTIC_RUMBLE_MIN_STRENGTH) left = 0;
    if (right <= HAPTIC_RUMBLE_MIN_STRENGTH) right = 0;
    uint32_t strength = left > right ? left : right;
    if (strength > 65535) strength = 65535;

    pthread_mutex_lock(&ctx->state_mutex);
    ctx->haptic_frames++;
    if (strength > 0) {
        if (strength < (1u << 9)) strength = 1u << 9;   // chiaki-ng's minimum
        if (ctx->haptic_count == 32) {                    // drop oldest
            ctx->haptic_head = (ctx->haptic_head + 1) % 32;
            ctx->haptic_count--;
        }
        ctx->haptic_queue[(ctx->haptic_head + ctx->haptic_count) % 32] = (uint16_t)strength;
        ctx->haptic_count++;
    }
    pthread_mutex_unlock(&ctx->state_mutex);
}

void input_fini(InputContext *ctx)
{
    if (!ctx) return;
    ctx->running = false;
    pthread_join(ctx->reader_thread, NULL);
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (ctx->pads[i].fd >= 0) {
            pad_output_close(&ctx->pads[i].out);
            ioctl(ctx->pads[i].fd, EVIOCGRAB, 0);
            close(ctx->pads[i].fd);
        }
    }
    if (ctx->inotify_fd >= 0) close(ctx->inotify_fd);
    pthread_mutex_destroy(&ctx->state_mutex);
    free(ctx);
}

// ── TV remote handler (SDL keyboard events only) ───────────────────────────────
// Gamepad events are handled entirely in evdev_reader() above.
// This function only processes SDL_KEYDOWN/KEYUP from the TV remote.
// WEBOS_KEY_BACK is intentionally absent — it exits the app (handled in main.c).
void input_handle_event(SDL_Event *ev, InputContext *ctx, ChiakiSession *session)
{
    if (!ctx) return;
    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP) return;

    bool pressed = (ev->type == SDL_KEYDOWN);
    uint32_t btn = 0;
    SDL_Keycode key = ev->key.keysym.sym;

    if      (key == SDLK_UP    || (int)key == WEBOS_KEY_UP)    btn = CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    else if (key == SDLK_DOWN  || (int)key == WEBOS_KEY_DOWN)  btn = CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    else if (key == SDLK_LEFT  || (int)key == WEBOS_KEY_LEFT)  btn = CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    else if (key == SDLK_RIGHT || (int)key == WEBOS_KEY_RIGHT) btn = CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    else if (key == SDLK_RETURN|| (int)key == WEBOS_KEY_OK)    btn = CHIAKI_CONTROLLER_BUTTON_CROSS;
    else if ((int)key == WEBOS_KEY_RED)                        btn = CHIAKI_CONTROLLER_BUTTON_BOX;
    else if ((int)key == WEBOS_KEY_GREEN)                      btn = CHIAKI_CONTROLLER_BUTTON_CROSS;
    else if ((int)key == WEBOS_KEY_BLUE)                       btn = CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    else if ((int)key == WEBOS_KEY_YELLOW)                     btn = CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    else return;

    pthread_mutex_lock(&ctx->state_mutex);
    if (pressed) ctx->remote_state.buttons |= btn;
    else         ctx->remote_state.buttons &= ~btn;
    publish_locked(ctx, session, false);
    pthread_mutex_unlock(&ctx->state_mutex);
}
