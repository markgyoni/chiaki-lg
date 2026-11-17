#include "input.h"
#include "app_log.h"
#include "pad_diag.h"
#include "pad_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <linux/input.h>

// ── Fallback defines for old kernel headers (pre-3.16 webOS sysroot) ──────────
// These gamepad layout aliases were added in Linux 3.16.
// The webOS toolchain sysroot ships with older headers that only have BTN_A/B/X/Y.
#ifndef BTN_SOUTH
#define BTN_SOUTH    0x130   /* BTN_A  — Cross / A  */
#endif
#ifndef BTN_EAST
#define BTN_EAST     0x131   /* BTN_B  — Circle / B */
#endif
#ifndef BTN_NORTH
#define BTN_NORTH    0x133   /* BTN_X  — Square / X     (confusingly, kernel maps Xbox-X to NORTH) */
#endif
#ifndef BTN_WEST
#define BTN_WEST     0x134   /* BTN_Y  — Triangle / Y   (confusingly, kernel maps Xbox-Y to WEST)  */
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif
#ifndef BTN_RECORD
#define BTN_RECORD     0x167   /* Xbox Series X|S capture/share button */
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

// ── Per-device state ───────────────────────────────────────────────────────────
#define MAX_GAMEPADS 4

// ── Touchpad chord: Select+Start → Touchpad (for Xbox controllers) ──────────
// When Select or Start is pressed alone, we defer emitting SHARE/OPTIONS for
// up to CHORD_WINDOW_MS to see if the other button follows.  If both arrive
// within the window → emit TOUCHPAD instead.  BTN_RECORD (Xbox Series capture
// button) maps directly to TOUCHPAD with no deferral.
#define CHORD_WINDOW_MS 100

typedef struct {
    bool            select_held;    // physical Select is currently down
    bool            start_held;     // physical Start is currently down
    bool            chord_active;   // chord detected → TOUCHPAD is pressed
    uint16_t        pending_btn;    // BTN_SELECT or BTN_START (0 = none)
    struct timespec pending_ts;     // when the pending button was pressed
    uint32_t        tap_pending;    // chiaki button bit needing immediate release after tap
} ChordState;

typedef struct {
    int  fd;
    char path[64];
    int  abs_min[ABS_CNT];
    int  abs_max[ABS_CNT];
    int  abs_logged[ABS_CNT];   // last value logged in input_debug mode
    ChordState chord;
    PadProfile profile;         // evdev code → chiaki mapping for this device
} GamepadDev;

struct InputContext {
    ChiakiControllerState state;
    pthread_mutex_t       state_mutex;

    GamepadDev    pads[MAX_GAMEPADS];
    int           num_pads;

    pthread_t     reader_thread;
    volatile bool running;
    bool          debug;
    char          profile_override[32];

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

// ── Monotonic clock helper ─────────────────────────────────────────────────────
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

static uint8_t normalize_trigger(int val, int min, int max)
{
    if (max <= min) return 0;
    if (val <= min) return 0;
    if (val >= max) return 255;
    return (uint8_t)(((int64_t)(val - min) * 255) / (max - min));
}

// ── Map evdev EV_ABS → chiaki sticks/triggers/dpad ────────────────────────────
static void apply_abs_event(ChiakiControllerState *st, GamepadDev *pad,
                             uint16_t code, int32_t val)
{
    if (code >= ABS_CNT) return;
    int min = pad->abs_min[code];
    int max = pad->abs_max[code];

    // Which evdev axis is which stick/trigger depends on the driver; the
    // per-controller profile (src/pad_profile.c) says.
    switch (pad_profile_axis(&pad->profile, code)) {
    case PAD_AXIS_LX: st->left_x  = normalize_axis(val, min, max); break;
    case PAD_AXIS_LY: st->left_y  = normalize_axis(val, min, max); break;
    case PAD_AXIS_RX: st->right_x = normalize_axis(val, min, max); break;
    case PAD_AXIS_RY: st->right_y = normalize_axis(val, min, max); break;
    case PAD_AXIS_L2: st->l2_state = normalize_trigger(val, min, max); break;
    case PAD_AXIS_R2: st->r2_state = normalize_trigger(val, min, max); break;
    case PAD_AXIS_HATX:
        st->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT |
                         CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT);
        if (val < 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
        if (val > 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
        break;
    case PAD_AXIS_HATY:
        st->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_DPAD_UP |
                         CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN);
        if (val < 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
        if (val > 0) st->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
        break;
    default: break;
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

        for (int i = 0; i < ctx->num_pads; i++) {
            if (ctx->pads[i].fd >= 0) {
                FD_SET(ctx->pads[i].fd, &rfds);
                if (ctx->pads[i].fd > maxfd)
                    maxfd = ctx->pads[i].fd;
            }
        }

        if (maxfd < 0) { usleep(50000); continue; }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 }; // 20ms for chord responsiveness
        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        // ── Check chord timeouts on every iteration (even if no events) ──────
        for (int i = 0; i < ctx->num_pads; i++) {
            ChordState *ch = &ctx->pads[i].chord;
            if (ch->pending_btn && ms_elapsed(&ch->pending_ts) >= CHORD_WINDOW_MS) {
                pthread_mutex_lock(&ctx->state_mutex);
                chord_flush_pending(&ctx->state, ch);
                ChiakiControllerState snap = ctx->state;
                pthread_mutex_unlock(&ctx->state_mutex);
                if (ctx->session)
                    chiaki_session_set_controller_state(ctx->session, &snap);
            }
        }

        if (sel <= 0) continue;

        for (int i = 0; i < ctx->num_pads; i++) {
            if (ctx->pads[i].fd < 0) continue;
            if (!FD_ISSET(ctx->pads[i].fd, &rfds)) continue;

            struct input_event ev;
            ssize_t n = read(ctx->pads[i].fd, &ev, sizeof(ev));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                app_log("[INPUT] Gamepad %s disconnected\n", ctx->pads[i].path);
                close(ctx->pads[i].fd);
                ctx->pads[i].fd = -1;
                continue;
            }
            if (n != sizeof(ev)) continue;

            if (ctx->debug) {
                GamepadDev *pd = &ctx->pads[i];
                const char *dev = strrchr(pd->path, '/') + 1;
                if (ev.type == EV_KEY) {
                    const char *nm = pad_diag_key_name(ev.code);
                    app_log_always("[PADEV] %s KEY 0x%03x %s %d\n", dev, ev.code,
                                   nm ? nm : "?", ev.value);
                } else if (ev.type == EV_ABS && ev.code < ABS_CNT &&
                           pad_diag_abs_significant(ev.value, pd->abs_min[ev.code],
                                                    pd->abs_max[ev.code],
                                                    &pd->abs_logged[ev.code])) {
                    const char *nm = pad_diag_abs_name(ev.code);
                    app_log_always("[PADEV] %s ABS 0x%02x %s %d\n", dev, ev.code,
                                   nm ? nm : "?", ev.value);
                }
            }

            bool changed = false;
            pthread_mutex_lock(&ctx->state_mutex);

            if      (ev.type == EV_KEY) { changed = apply_key_event(&ctx->state, &ctx->pads[i], ev.code, ev.value); }
            else if (ev.type == EV_ABS) { apply_abs_event(&ctx->state, &ctx->pads[i], ev.code, ev.value); changed = true; }

            ChiakiControllerState snap = ctx->state;
            pthread_mutex_unlock(&ctx->state_mutex);

            if (changed && ctx->session)
                chiaki_session_set_controller_state(ctx->session, &snap);

            // Handle deferred tap release: button was tapped+released while
            // still pending — we sent the press above, now immediately release.
            if (ctx->pads[i].chord.tap_pending) {
                pthread_mutex_lock(&ctx->state_mutex);
                ctx->state.buttons &= ~ctx->pads[i].chord.tap_pending;
                ctx->pads[i].chord.tap_pending = 0;
                ChiakiControllerState snap2 = ctx->state;
                pthread_mutex_unlock(&ctx->state_mutex);
                if (ctx->session)
                    chiaki_session_set_controller_state(ctx->session, &snap2);
            }
        }
    }

    return NULL;
}

// ── Check if evdev node is a physical gamepad ──────────────────────────────────
static bool is_gamepad(int fd, const char *name)
{
    // Skip the virtual LGE/LG mirrored devices that webOS hidd creates.
    // These are system-managed views of the physical controller; hidd still
    // intercepts B→Back / A→OK at the protocol level for these nodes.
    // The physical evdev node (e.g., "Sony DualSense") is what we want.
    if (strncmp(name, "LGE", 3) == 0 || strncmp(name, "LG ", 3) == 0) {
        app_log("[INPUT] Ignoring virtual LG device: \"%s\"\n", name);
        return false;
    }

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

    bool has_sticks = EVDEV_BITS_TEST(absbits, ABS_X) && EVDEV_BITS_TEST(absbits, ABS_Y);
    if (!has_sticks) {
        app_log("[INPUT] Skipping \"%s\" - no analog sticks (ABS_X/Y)\n", name);
        return false;
    }
    return true;
}

// ── Open and exclusively grab all physical gamepads ───────────────────────────
static void open_gamepads(InputContext *ctx)
{
    ctx->num_pads = 0;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        ctx->pads[i].fd = -1;
        ctx->pads[i].path[0] = '\0';
        memset(&ctx->pads[i].chord, 0, sizeof(ChordState));
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        app_log("[INPUT] opendir(/dev/input) failed: %s\n", strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && ctx->num_pads < MAX_GAMEPADS) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char devname[256] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(devname) - 1), devname);

        if (!is_gamepad(fd, devname)) { close(fd); continue; }

        // Real controllers sit behind a kernel HID/USB driver.  LG's virtual
        // remote/IME nodes ("Smart Remote RCU Input", "LGE Network Input")
        // have none, yet advertise every key and axis.
        char drv[64];
        pad_diag_evdev_driver(path, drv, sizeof(drv));
        if (!drv[0]) {
            app_log("[INPUT] Skipping \"%s\" - no kernel driver (virtual device)\n", devname);
            close(fd);
            continue;
        }

        GamepadDev *pad = &ctx->pads[ctx->num_pads];

        // Read axis ranges for proper normalization
        int abs_value[ABS_CNT];
        memset(abs_value, 0, sizeof(abs_value));
        for (int axis = 0; axis < ABS_CNT; axis++) {
            struct input_absinfo info;
            pad->abs_min[axis] = pad->abs_max[axis] = pad->abs_logged[axis] = 0;
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

        // EVIOCGRAB: take exclusive ownership.
        // Once grabbed, webOS's hidd daemon and Wayland compositor can no longer
        // see events from this device — B→Back, A→OK, Guide→Home system
        // interceptions are all suppressed.  Only our process receives events.
        //
        // Because SDL also opens /dev/input/event* for its joystick subsystem,
        // SDL's fd will stop receiving events after our grab. That is intentional:
        // we read and route events ourselves in evdev_reader(), completely
        // bypassing SDL's SDL_CONTROLLERBUTTON path.
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            app_log_always("[INPUT] EVIOCGRAB FAILED for \"%s\" %s: %s\n",
                           devname, path, strerror(errno));
            app_log_always("[INPUT] System button intercept (B=Back, A=OK) may still occur\n");
        } else {
            app_log("[INPUT] EVIOCGRAB OK — exclusive control of \"%s\" (%s)\n",
                    devname, path);
        }

        snprintf(pad->path, sizeof(pad->path), "%s", path);
        pad->fd = fd;
        ctx->num_pads++;

        app_log_always("[INPUT] Opened gamepad: \"%s\" @ %s bus=0x%02x vid=%04x pid=%04x driver=%s "
                       "profile=%s (%s)%s\n",
                       devname, path, id.bustype, id.vendor, id.product, drv,
                       pad->profile.id, pad->profile.description,
                       pad->profile.chord_touchpad ? " touchpad=Select+Start chord" : "");
    }
    closedir(dir);

    if (ctx->num_pads == 0)
        app_log_always("[INPUT] No physical gamepads found — TV remote only\n");
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

InputContext *input_init(bool debug, const char *profile_override)
{
    InputContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    chiaki_controller_state_set_idle(&ctx->state);
    pthread_mutex_init(&ctx->state_mutex, NULL);
    ctx->running = true;
    ctx->session = NULL;
    ctx->debug   = debug;
    snprintf(ctx->profile_override, sizeof(ctx->profile_override), "%s",
             profile_override ? profile_override : "auto");
    app_log_always("[INPUT] controller_profile=%s (choices: %s)\n",
                   ctx->profile_override, pad_profile_ids());

    log_controller_diagnostics(debug);
    open_gamepads(ctx);

    pthread_create(&ctx->reader_thread, NULL, evdev_reader, ctx);
    return ctx;
}

void input_set_session(InputContext *ctx, ChiakiSession *session)
{
    if (ctx) ctx->session = session;
}

void input_fini(InputContext *ctx)
{
    if (!ctx) return;
    ctx->running = false;
    pthread_join(ctx->reader_thread, NULL);
    for (int i = 0; i < ctx->num_pads; i++) {
        if (ctx->pads[i].fd >= 0) {
            ioctl(ctx->pads[i].fd, EVIOCGRAB, 0);
            close(ctx->pads[i].fd);
        }
    }
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
    if (pressed) ctx->state.buttons |= btn;
    else         ctx->state.buttons &= ~btn;
    ChiakiControllerState snap = ctx->state;
    pthread_mutex_unlock(&ctx->state_mutex);

    chiaki_session_set_controller_state(session, &snap);
}
