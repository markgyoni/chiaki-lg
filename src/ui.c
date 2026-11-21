#include "ui.h"
#include "keep_awake.h"
#include "config.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>

#include "ui_text.h"
#include "input.h"
#include "ui_padtest.h"
#include "app_log.h"
#include "ui_common.h"

/* ── Gamepad → menu navigation ───────────────────────────────────────────────
 * Gamepads are read by input.c (grabbed, so webOS never sees them).  Turn
 * their presses into the same key events the TV remote produces: d-pad or
 * left stick = arrows (with auto-repeat), Cross = OK, Circle = Back. */
#define NAV_REPEAT_DELAY_MS 400
#define NAV_REPEAT_RATE_MS  120
#define NAV_STICK_THRESHOLD 20000

static void push_key(SDL_Keycode sym)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
}

/* ── Pointer (LG Magic Remote) and menu keys ─────────────────────────────── */
#define MAX_HITS 96
typedef struct { int id; SDL_Rect rc; } HitRect;
static HitRect g_hits[MAX_HITS];
static int     g_nhits;
static int     g_hover = -1;
static bool    g_hover_new, g_last_click;
static Uint32  g_click_ms, g_enter_ms;

void ui_hit_begin(void) { g_nhits = 0; }

void ui_hit_add(int id, int x, int y, int w, int h)
{
    if (g_nhits < MAX_HITS) g_hits[g_nhits++] = (HitRect){ id, { x, y, w, h } };
}

static int hit_test(int x, int y)
{
    for (int i = g_nhits - 1; i >= 0; i--) {        /* last drawn is on top */
        const SDL_Rect *rc = &g_hits[i].rc;
        if (x >= rc->x && x < rc->x + rc->w && y >= rc->y && y < rc->y + rc->h) return g_hits[i].id;
    }
    return -1;
}

void ui_pointer_reset(void) { g_hover = -1; g_hover_new = false; g_last_click = false; }

int ui_pointer_hover(void)
{
    if (!g_hover_new) return -1;
    g_hover_new = false;
    return g_hover;
}

bool ui_pointer_clicked(void) { return g_last_click; }

bool ui_is_back_key(SDL_Keycode k)
{
    return k == SDLK_ESCAPE || k == WEBOS_KEY_BACK || k == UI_KEY_WEBOS_BACK || k == WEBOS_KEY_RED;
}

/* The remote's Back arrives with keycode 0 and the webOS scancode 482. */
bool ui_is_back_event(const SDL_Event *ev)
{
    return ev->type == SDL_KEYDOWN &&
           (ev->key.keysym.scancode == (SDL_Scancode)UI_SCANCODE_WEBOS_BACK || ui_is_back_key(ev->key.keysym.sym));
}

/* Pointer visibility, like LG's own apps: a direction key hides it at once,
 * and it hides after POINTER_IDLE_MS without remote movement.  Moving the
 * Magic Remote brings it back (webOS reports that as scancode 484 / motion). */
#define POINTER_IDLE_MS 8000
#define SC_CURSOR_SHOW  484   /* SDL_WEBOS_SCANCODE_CURSOR_SHOW */
#define SC_CURSOR_HIDE  485
static bool   g_ptr_visible = true;
static Uint32 g_ptr_active_ms;

static void pointer_set_visible(bool v)
{
    if (v == g_ptr_visible) return;
    g_ptr_visible = v;
    SDL_webOSCursorVisibility(v ? SDL_TRUE : SDL_FALSE);   /* return value is meaningless here */
    app_log("[UI] pointer %s\n", v ? "shown" : "hidden");
}

void ui_pointer_event(const SDL_Event *ev)
{
    Uint32 now = SDL_GetTicks() | 1u;
    switch (ev->type) {
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        g_ptr_active_ms = now;
        pointer_set_visible(true);
        break;
    case SDL_KEYDOWN: {
        int sc = (int)ev->key.keysym.scancode;
        SDL_Keycode k = ev->key.keysym.sym;
        if (sc == SC_CURSOR_SHOW) { g_ptr_visible = true; g_ptr_active_ms = now; }   /* webOS showed it */
        else if (sc == SC_CURSOR_HIDE) g_ptr_visible = false;                       /* webOS hid it */
        else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) pointer_set_visible(false);
        break;
    }
    }
}

void ui_pointer_tick(void)
{
    Uint32 now = SDL_GetTicks() | 1u;
    if (!g_ptr_active_ms) g_ptr_active_ms = now;     /* start counting at launch */
    if (g_ptr_visible && now - g_ptr_active_ms >= POINTER_IDLE_MS) pointer_set_visible(false);
}

SDL_Keycode ui_menu_key(const SDL_Event *ev)
{
    Uint32 now = SDL_GetTicks();
    ui_pointer_event(ev);
    switch (ev->type) {
    case SDL_KEYDOWN: {
        SDL_Keycode k = ev->key.keysym.sym;
        int sc = (int)ev->key.keysym.scancode;
        if (sc == 484 || sc == 485) { g_hover = -1; return 0; }   /* webOS pointer shown / hidden */
        if (ui_is_back_event(ev)) return SDLK_ESCAPE;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            if (g_click_ms && now - g_click_ms < 250) return 0;  /* the same press as a click */
            g_enter_ms = now;
        }
        g_last_click = false;
        return k;
    }
    case SDL_MOUSEMOTION: {
        int id = hit_test(ev->motion.x, ev->motion.y);
        if (id >= 0 && id != g_hover) { g_hover = id; g_hover_new = true; }
        return 0;
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (ev->button.button != SDL_BUTTON_LEFT) return 0;
        if (g_enter_ms && now - g_enter_ms < 250) return 0;      /* already handled as OK */
        int id = hit_test(ev->button.x, ev->button.y);
        if (id < 0) return 0;
        g_hover = id;
        g_hover_new = true;
        g_click_ms = now;
        g_last_click = true;
        return SDLK_RETURN;
    }
    case SDL_MOUSEWHEEL:
        g_last_click = false;
        return ev->wheel.y > 0 ? SDLK_UP : ev->wheel.y < 0 ? SDLK_DOWN : 0;
    }
    return 0;
}

void pad_nav_poll(struct InputContext *in)
{
    static uint32_t prev;
    static Uint32 repeat_at;
    ui_pointer_tick();                   /* every menu frame comes through here */
    if (!in) return;

    ChiakiControllerState st;
    input_get_state(in, &st);
    uint32_t b = st.buttons;
    if (st.left_y < -NAV_STICK_THRESHOLD) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    if (st.left_y >  NAV_STICK_THRESHOLD) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    if (st.left_x < -NAV_STICK_THRESHOLD) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    if (st.left_x >  NAV_STICK_THRESHOLD) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;

    static const struct { uint32_t btn; SDL_Keycode key; bool repeat; } MAP[] = {
        { CHIAKI_CONTROLLER_BUTTON_DPAD_UP,    SDLK_UP,     true  },
        { CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN,  SDLK_DOWN,   true  },
        { CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT,  SDLK_LEFT,   true  },
        { CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT, SDLK_RIGHT,  true  },
        { CHIAKI_CONTROLLER_BUTTON_CROSS,      SDLK_RETURN, false },
        { CHIAKI_CONTROLLER_BUTTON_MOON,       SDLK_ESCAPE, false },
    };
    Uint32 now = SDL_GetTicks();
    bool held_dir = false;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        bool down = (b & MAP[i].btn) != 0, was = (prev & MAP[i].btn) != 0;
        if (down && !was) {
            push_key(MAP[i].key);
            if (MAP[i].repeat) repeat_at = now + NAV_REPEAT_DELAY_MS;
        } else if (down && MAP[i].repeat && !held_dir && (Sint32)(now - repeat_at) >= 0) {
            push_key(MAP[i].key);
            repeat_at = now + NAV_REPEAT_RATE_MS;
        }
        if (down && MAP[i].repeat) held_dir = true;
    }
    if (b != prev) ui_note_pad_activity();
    prev = b;
    ui_keep_awake_tick();
}

/* ── Keep the TV awake while a gamepad is used ───────────────────────────────
 * Gamepads are grabbed, so webOS never sees their input and starts its
 * screensaver after a couple of idle minutes even while someone navigates
 * the menus with a controller.  While there was gamepad activity in the last
 * two minutes keep_awake_poke() resets webOS's screensaver/standby timers (SDL's own
 * SDL_DisableScreenSaver does not reach them on webOS 6). */
#define KEEP_AWAKE_MS 120000
static Uint32 g_pad_activity_ms;
static bool   g_screensaver_blocked;

void ui_note_pad_activity(void)
{
    g_pad_activity_ms = SDL_GetTicks() | 1u;
}

void ui_keep_awake_tick(void)
{
    bool want = g_pad_activity_ms && SDL_GetTicks() - g_pad_activity_ms < KEEP_AWAKE_MS;
    if (want) keep_awake_poke();             /* rate-limited, non-blocking */
    if (want == g_screensaver_blocked) return;
    g_screensaver_blocked = want;
    app_log_always("[UI] screensaver %s (gamepad %s)\n", want ? "blocked" : "allowed",
                   want ? "in use" : "idle for 2 min");
}


/* ── Primitive drawing helpers ───────────────────────────────────────────── */

void set_color(SDL_Renderer *r, int R, int G, int B, int A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

void fill_rect(SDL_Renderer *r, int x, int y, int w, int h)
{
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

/* Drawing size: the renderer's logical size (1920x1080, set in main.c and
 * scaled to whatever the TV's UI plane is), else its real output size. */
static void ui_screen_size(SDL_Renderer *r, int *w, int *h)
{
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    if (lw > 0 && lh > 0) { *w = lw; *h = lh; return; }
    (void)SDL_GetRendererOutputSize(r, w, h);
}

static void draw_rect_outline(SDL_Renderer *r, int x, int y, int w, int h)
{
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

/* Text goes through ui_text (LG TrueType fonts, bitmap fallback).  `scale`
 * is the old bitmap-font scale: glyphs 7*scale tall, lines 9*scale apart. */
static int scale_px(int scale) { return scale <= 1 ? 16 : 11 * scale; }

static SDL_Color current_color(SDL_Renderer *r)
{
    SDL_Color c;
    SDL_GetRenderDrawColor(r, &c.r, &c.g, &c.b, &c.a);
    return c;
}

void draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale)
{
    ui_text_init(r);              /* idempotent; first call loads the fonts */
    int px = scale_px(scale);
    SDL_Color c = current_color(r);
    /* centre each line on where the old 7*scale glyph box was */
    int top_adj = (7 * scale) / 2 - ui_text_line_height(px) / 2;
    char line[512];
    const char *p = text;
    while (p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = '\0';
        ui_text_draw(r, x, y + top_adj, line, px, UI_FONT_REGULAR, c, UI_ALIGN_LEFT);
        y += ui_text_line_height(px);
        p = nl ? nl + 1 : NULL;
    }
}

int text_width(const char *text, int scale)
{
    char line[512];
    size_t len = strcspn(text, "\n");
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, text, len); line[len] = '\0';
    return ui_text_width(line, scale_px(scale), UI_FONT_REGULAR);
}
static void draw_text_centered(SDL_Renderer *r, int cx, int y,
                                const char *text, int scale)
{
    draw_text(r, cx - text_width(text, scale) / 2, y, text, scale);
}

/* Rounded rect: a middle band plus one scanline per corner row, inset along
 * the circle.  radius = h/2 gives a pill. */
/* The app's "Couch Pad" mark (docs/branding/logo.svg) as a size x size tile:
 * a controller with a signal above it, drawn on the 200-unit grid of the SVG. */
void ui_draw_logo(SDL_Renderer *r, int x, int y, int size)
{
    const float s = size / 200.0f;
#define LX(v) (x + (int)lroundf((v) * s))
#define LY(v) (y + (int)lroundf(((v) + 7) * s))
#define LS(v) ((int)lroundf((v) * s))
    set_color(r, 0x00, 0x43, 0x9c, 0xff);
    fill_rounded_rect(r, x, y, size, size, size / 6);
    /* two signal arcs over the pad, as rows of dots (round caps for free) */
    /* the outer arc is white at 60% over the tile, as a solid colour so the
     * overlapping dots don't darken */
    static const struct { float rad, w; Uint8 red, green, blue; } ARCS[] = { { 19.8f, 9, 0xff, 0xff, 0xff }, { 33.9f, 9, 0x99, 0xb4, 0xd7 } };
    for (int i = 0; i < 2; i++) {
        set_color(r, ARCS[i].red, ARCS[i].green, ARCS[i].blue, 0xff);
        int d = LS(ARCS[i].w);
        if (d < 2) d = 2;
        for (int deg = 225; deg <= 315; deg += 3) {
            float rad = deg * 3.14159265f / 180.0f;
            int cx = LX(100 + ARCS[i].rad * cosf(rad)), cy = LY(68 + ARCS[i].rad * sinf(rad));
            fill_rounded_rect(r, cx - d / 2, cy - d / 2, d, d, d / 2);
        }
    }
    set_color(r, 0xff, 0xff, 0xff, 0xff);
    fill_rounded_rect(r, LX(30), LY(72), LS(140), LS(62), LS(31));
    fill_rounded_rect(r, LX(30), LY(104), LS(48), LS(48), LS(24));
    fill_rounded_rect(r, LX(122), LY(104), LS(48), LS(48), LS(24));
    set_color(r, 0x00, 0x43, 0x9c, 0xff);
    fill_rect(r, LX(51), LY(99), LS(24), LS(8));
    fill_rect(r, LX(59), LY(91), LS(8), LS(24));
    fill_rounded_rect(r, LX(143), LY(104), LS(14), LS(14), LS(7));
    set_color(r, COL_ACCENT);
    fill_rounded_rect(r, LX(127), LY(89), LS(14), LS(14), LS(7));
#undef LX
#undef LY
#undef LS
}

void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) { fill_rect(r, x, y, w, h); return; }
    fill_rect(r, x, y + radius, w, h - radius * 2);
    for (int i = 0; i < radius; i++) {
        float dy = (float)radius - (float)i - 0.5f;
        int inset = radius - (int)(sqrtf((float)(radius * radius) - dy * dy) + 0.5f);
        fill_rect(r, x + inset, y + i,         w - inset * 2, 1);
        fill_rect(r, x + inset, y + h - 1 - i, w - inset * 2, 1);
    }
}

/* 1px outline matching fill_rounded_rect's shape (no pixel drawn twice, so
 * translucent colours stay even). */
void stroke_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) { draw_rect_outline(r, x, y, w, h); return; }
    fill_rect(r, x,         y + radius, 1, h - radius * 2);
    fill_rect(r, x + w - 1, y + radius, 1, h - radius * 2);
    int prev = w / 2;                          /* row 0 runs to the middle: top edge */
    for (int i = 0; i < radius; i++) {
        float dy = (float)radius - (float)i - 0.5f;
        int inset = radius - (int)(sqrtf((float)(radius * radius) - dy * dy) + 0.5f);
        int len = prev - inset;
        if (len < 1) len = 1;
        int top = y + i, bot = y + h - 1 - i;
        if (i == 0) {                          /* straight top/bottom edge */
            fill_rect(r, x + inset, top, w - inset * 2, 1);
            fill_rect(r, x + inset, bot, w - inset * 2, 1);
        } else {
            fill_rect(r, x + inset,             top, len, 1);
            fill_rect(r, x + w - inset - len,   top, len, 1);
            fill_rect(r, x + inset,             bot, len, 1);
            fill_rect(r, x + w - inset - len,   bot, len, 1);
        }
        prev = inset;
    }
}

/* Filled horizontal line (1px tall) */
void hline(SDL_Renderer *r, int x, int y, int w)
{
    fill_rect(r, x, y, w, 1);
}

/* ── Button ─────────────────────────────────────────────────────────────── */

void draw_button(SDL_Renderer *r, int x, int y, int w, int h,
                        const char *label, bool focused, bool enabled)
{
    int radius = 10;
    const int scale = 3;               /* same size as the settings rows */
    int text_y = y + (h - 7 * scale) / 2;

    if (!enabled) {
        /* Disabled — very faint */
        set_color(r, COL_BORDER_DIM);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_TEXT_FAINT);
        draw_text_centered(r, x + w / 2, text_y, label, scale);
        return;
    }

    if (focused) {
        /* Focused — white ring + accent fill, like the launcher buttons */
        set_color(r, COL_TEXT);
        fill_rounded_rect(r, x - 3, y - 3, w + 6, h + 6, radius + 3);
        set_color(r, COL_ACCENT);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_TEXT);
        draw_text_centered(r, x + w / 2, text_y, label, scale);
    } else {
        /* Normal enabled */
        set_color(r, COL_CARD);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_BORDER);
        stroke_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_TEXT);
        draw_text_centered(r, x + w / 2, text_y, label, scale);
    }
}

/* ── Launcher (TV layout: big text, two columns) ────────────────────────── */


/* Large button with TrueType label.  primary = accent-coloured when idle too. */
void big_button(SDL_Renderer *r, int x, int y, int w, int h, const char *label,
                       bool focused, bool enabled, bool primary)
{
    if (focused) {
        set_color(r, COL_TEXT);
        fill_rounded_rect(r, x - 4, y - 4, w + 8, h + 8, 16);
    }
    if (!enabled)        set_color(r, COL_BORDER_DIM);
    else if (focused)    set_color(r, COL_ACCENT);
    else if (primary)    set_color(r, COL_ACCENT_DARK);
    else                 set_color(r, COL_CARD);
    fill_rounded_rect(r, x, y, w, h, 14);
    SDL_Color tc = enabled ? C(COL_TEXT) : C(COL_TEXT_FAINT);
    ui_text_draw(r, x + w / 2, y + (h - ui_text_line_height(38)) / 2, label, 38,
                 UI_FONT_SEMIBOLD, tc, UI_ALIGN_CENTER);
}


/* ══════════════════════════════════════════════════════════════════════════
 * App settings (controllers, stream overlay, logging).  Per-console stream
 * settings live in the console settings screen (ui_home.c).
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    /* left column */
    SET_PSN = 0,          /* ACCOUNT */
    SET_STATS_CORNER,     /* STREAM */
    SET_LOG_LEVEL,        /* ADVANCED */
    SET_INPUT_DEBUG,
    /* right column */
    SET_PROFILE,          /* CONTROLLERS */
    SET_RUMBLE,
    SET_DS_HID,
    SET_MOTION,
    SET_TRIGGERS,
    SET_BT_PATCH,
    SET_RETURN,
    SET_COUNT
} SettingsFocus;

static const char *PROFILE_OPTIONS[] = {"auto", "sony_generic", "sony_kernel", "xbox_bt", "xpad", "generic"};
static const char *PROFILE_LABELS[]  = {"Automatic", "PlayStation (HID)", "PlayStation (kernel)",
                                        "Xbox (Bluetooth)", "Xbox (USB)", "Generic"};
#define N_PROFILES ((int)(sizeof(PROFILE_OPTIONS) / sizeof(PROFILE_OPTIONS[0])))

static const char *SETTING_HELP[SET_COUNT] = {
    [SET_PSN]           = "PSN accounts signed in on this TV. Each console is linked with one of them (also used for cloud wakeup). OK: sign in or out.",
    [SET_STATS_CORNER]  = "Where the stream stats appear. Show or hide them with Up on the remote or Mute on the DualSense.",
    [SET_LOG_LEVEL]     = "How much is written to /tmp/chiaki.log. Warning is enough unless you are debugging.",
    [SET_INPUT_DEBUG]   = "Log every raw controller event. Only for diagnosing controller problems.",
    [SET_PROFILE]       = "Button layout. Automatic picks the right one for each controller.",
    [SET_RUMBLE]        = "Pass vibration from games to your controller.",
    [SET_DS_HID]        = "Read the DualSense directly: touchpad, motion, adaptive triggers, lightbar.",
    [SET_MOTION]        = "Send DualSense gyro and accelerometer to the console (motion aiming).",
    [SET_TRIGGERS]      = "DualSense adaptive trigger resistance from games.",
    [SET_BT_PATCH]      = "Rooted TVs: fix the TV's Bluetooth so DualSense vibration, triggers and lightbar "
                          "work wirelessly. Nothing is changed on disk; a reboot undoes it.",
    [SET_RETURN]        = "Go back to the start screen.",
};

static const int  LOG_LEVEL_MASKS[]  = { 0, 16, 16|8, 16|8|4, 16|8|4|2, 16|8|4|2|1 };
static const char *LOG_LEVEL_LABELS[] = { "Off", "Error", "Warning", "Info", "Verbose", "Debug" };
static const char *CORNER_LABELS[] = { "Top left", "Top right", "Bottom left", "Bottom right" };

static int log_level_idx(int mask)
{
    for (int i = 5; i >= 0; i--)
        if ((mask & LOG_LEVEL_MASKS[i]) == LOG_LEVEL_MASKS[i] && (i == 0 ? mask == 0 : true)) return i;
    return 2;
}

const char *on_off(bool v) { return v ? "On" : "Off"; }

/* Settings row: label on the left, value chip on the right */
void ui_draw_option_row(SDL_Renderer *r, int x, int y, int w, int h,
                        const char *label, const char *value, bool focused)
{
    int radius = 8;
    if (focused) {
        set_color(r, COL_ACCENT_DARK);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_ACCENT);
        fill_rect(r, x, y + 2, 4, h - 4);
    } else {
        set_color(r, COL_CODE_BG);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_BORDER_DIM);
        stroke_rounded_rect(r, x, y, w, h, radius);
    }

    int scale = 3;
    int ty = y + (h - 7 * scale) / 2;
    if (focused) set_color(r, COL_TEXT); else set_color(r, COL_TEXT_DIM);
    draw_text(r, x + 20, ty, label, scale);

    if (value) {
        int vw = text_width(value, scale);
        if (focused) {
            set_color(r, COL_ACCENT);
            fill_rounded_rect(r, x + w - vw - 32, ty - 4, vw + 24, 7 * scale + 8, 6);
            set_color(r, COL_TEXT);
        } else {
            set_color(r, COL_CARD);
            fill_rounded_rect(r, x + w - vw - 32, ty - 4, vw + 24, 7 * scale + 8, 6);
            set_color(r, COL_TEXT_DIM);
        }
        draw_text(r, x + w - vw - 20, ty, value, scale);
    }
}

void ui_draw_header(SDL_Renderer *r, const char *title, const char *right_note)
{
    ui_hit_begin();                  /* every settings-style screen starts its frame here */
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
    set_color(r, COL_PANEL);
    fill_rect(r, 0, HDR_Y, SCREEN_W, HDR_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, HDR_Y + HDR_H - 1, SCREEN_W);
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, HDR_Y, 8, HDR_H);
    set_color(r, COL_TEXT);
    draw_text(r, 36, HDR_Y + 18, title, 4);
    if (right_note && right_note[0]) {
        set_color(r, COL_TEXT_DIM);
        draw_text(r, SCREEN_W - 48 - text_width(right_note, 2), HDR_Y + 30, right_note, 2);
    }
}

void ui_draw_help_footer(SDL_Renderer *r, const char *help, const char *keys, const char *msg)
{
    const int LX = 60, FOOT_Y = SCREEN_H - 70, FOOT_H = 70;
    int help_y = FOOT_Y - 116;
    set_color(r, COL_PANEL);
    fill_rounded_rect(r, LX, help_y, SCREEN_W - 2 * LX, 100, 12);
    set_color(r, COL_ACCENT);
    fill_rect(r, LX, help_y + 10, 4, 80);
    if (help && help[0])
        ui_text_draw_wrapped(r, LX + 28, help_y + 14, SCREEN_W - 2 * LX - 56, help, 30,
                             UI_FONT_REGULAR, C(COL_TEXT), 2);

    set_color(r, COL_PANEL);
    fill_rect(r, 0, FOOT_Y, SCREEN_W, FOOT_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, FOOT_Y, SCREEN_W);
    set_color(r, COL_TEXT_DIM);
    draw_text(r, 48, FOOT_Y + (FOOT_H - 14) / 2, keys, 2);
    if (msg && msg[0]) {
        bool bad = strstr(msg, "ailed") || strstr(msg, "not ");
        if (bad) set_color(r, COL_ERROR); else set_color(r, COL_SUCCESS);
        draw_text(r, SCREEN_W - 48 - text_width(msg, 2), FOOT_Y + (FOOT_H - 14) / 2, msg, 2);
    }
}

static void ui_render_settings_screen(SDL_Renderer *r, const AppConfig *cfg, int focus,
                                      const char *msg, int log_idx, int profile_idx)
{
    ui_draw_header(r, "App settings", "Saved automatically");

    const int ROW_H = 58, ROW_GAP = 8, GROUP_GAP = 18;
    const int COL_W = (SCREEN_W - 60 * 2 - 40) / 2;
    const int LX = 60, RX = LX + COL_W + 40;

#define GROUP(x, y, name) do { set_color(r, COL_ACCENT); draw_text(r, (x) + 4, (y), name, 2); (y) += 34; } while (0)
#define ROW(x, y, id, label, value) do { \
        ui_draw_option_row(r, (x), (y), COL_W, ROW_H, label, value, focus == (id)); \
        ui_hit_add((id), (x), (y), COL_W, ROW_H); \
        (y) += ROW_H + ROW_GAP; } while (0)

    int y = CONTENT_Y + 6;
    GROUP(LX, y, "ACCOUNT");
    char psn[64];
    if (cfg->n_psn_accounts == 0) snprintf(psn, sizeof(psn), "Not signed in");
    else if (cfg->n_psn_accounts == 1) snprintf(psn, sizeof(psn), "%s", cfg->psn_accounts[0].name);
    else snprintf(psn, sizeof(psn), "%d accounts", cfg->n_psn_accounts);
    ROW(LX, y, SET_PSN, "PSN accounts", psn);
    y += GROUP_GAP;
    GROUP(LX, y, "STREAM");
    ROW(LX, y, SET_STATS_CORNER, "Stats overlay position", CORNER_LABELS[cfg->stats_corner & 3]);
    y += GROUP_GAP;
    GROUP(LX, y, "ADVANCED");
    ROW(LX, y, SET_LOG_LEVEL, "Log level", LOG_LEVEL_LABELS[log_idx]);
    ROW(LX, y, SET_INPUT_DEBUG, "Controller debug log", on_off(cfg->input_debug));
    y += GROUP_GAP;
    set_color(r, COL_TEXT_DIM);
    ui_text_draw_wrapped(r, LX + 4, y, COL_W - 8,
                         "Resolution, frame rate, bitrate, codec, wake and rest mode are set per "
                         "console: select a console on the start screen and open its settings.",
                         26, UI_FONT_REGULAR, C(COL_TEXT_DIM), 4);

    y = CONTENT_Y + 6;
    GROUP(RX, y, "CONTROLLERS");
    ROW(RX, y, SET_PROFILE, "Button layout", PROFILE_LABELS[profile_idx]);
    ROW(RX, y, SET_RUMBLE, "Vibration", on_off(cfg->rumble));
    ROW(RX, y, SET_DS_HID, "DualSense enhanced", on_off(cfg->dualsense_hid));
    ROW(RX, y, SET_MOTION, "Motion controls", on_off(cfg->motion));
    ROW(RX, y, SET_TRIGGERS, "Adaptive triggers", on_off(cfg->trigger_effects));
    ROW(RX, y, SET_BT_PATCH, "Bluetooth fix (DualSense)", on_off(cfg->dualsense_bt_patch));
    y += GROUP_GAP;
    draw_button(r, RX, y, COL_W, ROW_H + 6, "Back", focus == SET_RETURN, true);
    ui_hit_add(SET_RETURN, RX, y, COL_W, ROW_H + 6);
#undef ROW
#undef GROUP

    ui_draw_help_footer(r, (focus >= 0 && focus < SET_COUNT) ? SETTING_HELP[focus] : "",
                        "Up/Down: move   Left/Right/OK: change   Back: return", msg);
}

void ui_run_settings(SDL_Renderer *renderer, AppConfig *cfg, const char *config_path,
                     struct InputContext *input)
{
    if (!renderer || !cfg) return;

    int log_idx = log_level_idx(cfg->log_level);
    int profile_idx = 0;
    for (int i = 0; i < N_PROFILES; i++)
        if (cfg->controller_profile && !strcmp(cfg->controller_profile, PROFILE_OPTIONS[i])) profile_idx = i;
    int focus = SET_PSN;
    bool input_changed = false, wants_exit = false;
    char msg[128] = {0};

    ui_pointer_reset();
    while (!wants_exit) {
        pad_nav_poll(input);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { wants_exit = true; continue; }
            SDL_Keycode sym = ui_menu_key(&ev);
            int hv = ui_pointer_hover();
            if (hv >= 0 && hv < SET_COUNT) focus = hv;
            if (!sym) continue;
            if (ui_is_back_key(sym)) { wants_exit = true; continue; }
            if      (sym == SDLK_UP   && focus > 0)          focus--;
            else if (sym == SDLK_DOWN && focus < SET_RETURN) focus++;
            else {
                bool fwd = (sym == SDLK_RIGHT || sym == SDLK_RETURN || sym == SDLK_KP_ENTER);
                bool back = (sym == SDLK_LEFT);
                if (!fwd && !back) continue;
                bool changed = true;
                switch (focus) {
                case SET_PSN:
                    if (sym != SDLK_RETURN && sym != SDLK_KP_ENTER) { changed = false; break; }
                    ui_psn_accounts(renderer, input, cfg, config_path);
                    changed = false;
                    msg[0] = '\0';
                    break;
                case SET_STATS_CORNER: cfg->stats_corner = (cfg->stats_corner + (fwd ? 1 : 3)) % 4; break;
                case SET_LOG_LEVEL:    log_idx = (log_idx + (fwd ? 1 : 5)) % 6; cfg->log_level = LOG_LEVEL_MASKS[log_idx]; break;
                case SET_INPUT_DEBUG:  cfg->input_debug = !cfg->input_debug; input_changed = true; break;
                case SET_PROFILE:      profile_idx = (profile_idx + (fwd ? 1 : N_PROFILES - 1)) % N_PROFILES;
                                       config_set_str(&cfg->controller_profile, PROFILE_OPTIONS[profile_idx]);
                                       input_changed = true; break;
                case SET_RUMBLE:       cfg->rumble = !cfg->rumble; input_changed = true; break;
                case SET_DS_HID:       cfg->dualsense_hid = !cfg->dualsense_hid; input_changed = true; break;
                case SET_MOTION:       cfg->motion = !cfg->motion; input_changed = true; break;
                case SET_TRIGGERS:     cfg->trigger_effects = !cfg->trigger_effects; input_changed = true; break;
                case SET_BT_PATCH:     cfg->dualsense_bt_patch = !cfg->dualsense_bt_patch; input_changed = true; break;
                case SET_RETURN:       changed = false; if (fwd) wants_exit = true; break;
                default:               changed = false; break;
                }
                if (changed)
                    snprintf(msg, sizeof(msg), "%s", config_save(cfg, config_path) ? "Saved." : "Failed to write config.json");
            }
        }
        if (wants_exit) break;
        ui_render_settings_screen(renderer, cfg, focus, msg, log_idx, profile_idx);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (input_changed && input) {
        InputOptions o = {
            .debug = cfg->input_debug, .profile_override = cfg->controller_profile,
            .rumble = cfg->rumble, .dualsense_hid = cfg->dualsense_hid, .motion = cfg->motion,
            .trigger_effects = cfg->trigger_effects, .dualsense_bt_patch = cfg->dualsense_bt_patch,
        };
        input_reconfigure(input, &o);
    }
    SDL_Delay(150);
    SDL_PumpEvents();
    { SDL_Event d; while (SDL_PollEvent(&d)) {} }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Loading screen
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_loading(SDL_Renderer *r, const char *base_text)
{
    ui_hit_begin();                  /* nothing to click while loading */
    int sw = SCREEN_W, sh = SCREEN_H;
    ui_screen_size(r, &sw, &sh);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, sw, sh);

    /* PS blue top accent bar */
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, 0, sw, 6);

    /* App branding in top-left */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, 40, 30, "CHIAKI-WEBOS", 3);

    /* Centre content */
    if (!base_text) base_text = "Loading";

    /* Animated spinner (8-frame dot ring around centre text) */
    int cx = sw / 2, cy = sh / 2;

    /* Spinner dots: 8 positions around a circle radius 42 */
    /* Approximate circle positions at radius 42 */
    static const int DOT_DX[8] = {  0, 30, 42, 30,  0, -30, -42, -30 };
    static const int DOT_DY[8] = {-42,-30,  0, 30, 42,  30,   0, -30 };
    int spin_frame = (int)((SDL_GetTicks() / 80) % 8);

    for (int i = 0; i < 8; i++) {
        int age = (8 + spin_frame - i) % 8;  /* 0 = newest (brightest) */
        int bright = 200 - age * 22;
        if (bright < 40) bright = 40;
        set_color(r, 0x00, (int)(bright * 0.6f), bright, 0xff);
        int dsz = (age == 0) ? 10 : 6;
        fill_rounded_rect(r,
            cx + DOT_DX[i] - dsz / 2,
            cy + DOT_DY[i] - dsz / 2,
            dsz, dsz, 3);
    }

    /* Animated message with dots */
    int dot_count = (int)((SDL_GetTicks() / 400) % 4);
    char msg[256];
    snprintf(msg, sizeof(msg), "%s%.*s", base_text, dot_count, "...");

    /* Centre on the text with all three dots so it doesn't shift as they animate. */
    int msg_scale = 4;
    char full[256];
    snprintf(full, sizeof(full), "%s...", base_text);
    int mw = text_width(full, msg_scale);
    set_color(r, COL_TEXT);
    draw_text(r, cx - mw / 2, cy + 70, msg, msg_scale);

    /* Subtitle */
    set_color(r, COL_TEXT_DIM);
    draw_text_centered(r, cx, cy + 116, "Please wait", 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stats overlay
 * ══════════════════════════════════════════════════════════════════════════ */

/* Lines are "LABEL<2+ spaces>value"; the first line is the title.  Labels go
 * in a dim left column, values in a second column.  corner: 0 top-left,
 * 1 top-right, 2 bottom-left, 3 bottom-right. */
void ui_render_stats_overlay(SDL_Renderer *r, const char *text, int corner)
{
    if (!r || !text || !text[0]) return;

    int sw = SCREEN_W, sh = SCREEN_H;
    ui_screen_size(r, &sw, &sh);
    ui_text_init(r);

    const int px = 26, title_px = 22, pad = 22, gap = 24, margin = 36;
    const int line_h = ui_text_line_height(px);

    /* split into title + (label, value) rows */
    char title[64] = "";
    char labels[16][48], values[16][160];
    int rows = 0, label_w = 0, value_w = 0;
    const char *p = text;
    for (int li = 0; p && rows < 16; li++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = '\0';
        p = nl ? nl + 1 : NULL;
        if (li == 0) { snprintf(title, sizeof(title), "%s", line); continue; }
        char *sep = strstr(line, "  ");
        if (sep) {
            *sep = '\0';
            char *v = sep + 2;
            while (*v == ' ') v++;
            snprintf(labels[rows], sizeof(labels[rows]), "%s", line);
            snprintf(values[rows], sizeof(values[rows]), "%s", v);
        } else {
            labels[rows][0] = '\0';
            snprintf(values[rows], sizeof(values[rows]), "%s", line);
        }
        int lw = ui_text_width(labels[rows], px, UI_FONT_REGULAR);
        int vw = ui_text_width(values[rows], px, UI_FONT_SEMIBOLD);
        if (lw > label_w) label_w = lw;
        if (vw > value_w) value_w = vw;
        rows++;
    }

    /* Keep the box still while numbers change: the value column is at least as
     * wide as a worst-case sample, and never shrinks during the app's run. */
    static int value_w_max;
    static const char *const WIDEST[] = {
        "3840x2160  H.265 HDR", "000.0 fps (target 120)", "000.0 Mbps  (audio 0.00)",
        "000 ms", "00.0 %   Frames lost: 00000", "000 ms (00 frames)   errors 000", "00:00:00",
    };
    for (size_t i = 0; i < sizeof(WIDEST) / sizeof(WIDEST[0]); i++) {
        int w = ui_text_width(WIDEST[i], px, UI_FONT_SEMIBOLD);
        if (w > value_w) value_w = w;
    }
    if (value_w > value_w_max) value_w_max = value_w;
    value_w = value_w_max;

    int title_h = ui_text_line_height(title_px) + 10;
    int box_w = pad * 2 + label_w + gap + value_w;
    int box_h = pad * 2 + title_h + rows * line_h;
    if (box_w > sw - 2 * margin) box_w = sw - 2 * margin;

    int x = (corner == 1 || corner == 3) ? sw - margin - box_w : margin;
    int y = (corner == 2 || corner == 3) ? sh - margin - box_h : margin;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    set_color(r, 0x00, 0x00, 0x00, 90);
    fill_rounded_rect(r, x + 4, y + 6, box_w, box_h, 18);
    set_color(r, 0x05, 0x0a, 0x18, 215);
    fill_rounded_rect(r, x, y, box_w, box_h, 18);
    set_color(r, 0x2a, 0x3a, 0x66, 255);
    stroke_rounded_rect(r, x, y, box_w, box_h, 18);

    SDL_Color accent = { 0x40, 0x9c, 0xff, 0xff };
    SDL_Color dim    = { 0x8a, 0x9a, 0xc0, 0xff };
    SDL_Color val    = { 0xec, 0xf2, 0xff, 0xff };
    ui_text_draw(r, x + pad, y + pad, title, title_px, UI_FONT_BOLD, accent, UI_ALIGN_LEFT);
    int ty = y + pad + title_h;
    for (int i = 0; i < rows; i++) {
        ui_text_draw(r, x + pad, ty, labels[i], px, UI_FONT_REGULAR, dim, UI_ALIGN_LEFT);
        ui_text_draw(r, x + pad + label_w + gap, ty, values[i], px, UI_FONT_SEMIBOLD, val, UI_ALIGN_LEFT);
        ty += line_h;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Startup hint overlay
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_hint(SDL_Renderer *r, const char *text, float opacity)
{
    if (!r || !text || !text[0] || opacity <= 0.0f) return;

    int sw = SCREEN_W, sh = SCREEN_H;
    ui_screen_size(r, &sw, &sh);
    (void)sh;

    const int scale = 2;
    const int pad   = 14;
    int tw = text_width(text, scale);

    int box_w = tw + pad * 2 + 8;
    int box_h = 7 * scale + pad * 2;
    int x = 24, y = 24;

    if (opacity > 1.0f) opacity = 1.0f;
    uint8_t alpha_bg   = (uint8_t)(180.0f * opacity);
    uint8_t alpha_text = (uint8_t)(255.0f * opacity);
    uint8_t alpha_acc  = (uint8_t)(220.0f * opacity);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    set_color(r, 0x06, 0x0e, 0x24, alpha_bg);
    fill_rounded_rect(r, x, y, box_w, box_h, box_h / 2);

    set_color(r, 0x00, 0x72, 0xff, alpha_acc);
    fill_rect(r, x, y + 3, 4, box_h - 6);

    set_color(r, 0xec, 0xf2, 0xff, alpha_text);
    draw_text(r, x + pad + 8, y + pad, text, scale);
}

static char g_notice[256];

void ui_set_launcher_notice(const char *text)
{
    snprintf(g_notice, sizeof(g_notice), "%s", text ? text : "");
}

const char *ui_take_launcher_notice(void)
{
    static char out[256];
    snprintf(out, sizeof(out), "%s", g_notice);
    g_notice[0] = '\0';
    return out;
}

/* ══════════════════════════════════════════════════════════════════════════
 * "Put the console in rest mode?" — shown over the stream when leaving it
 * ══════════════════════════════════════════════════════════════════════════ */

bool ui_ask_rest_mode(SDL_Renderer *r, struct InputContext *input)
{
    if (!r) return false;
    ui_text_init(r);
    int focus = 1;                             /* 0 = Rest mode, 1 = Keep on */
    int answer = -1;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { /* drop the keys that led here */ }

    ui_pointer_reset();
    while (answer < 0) {
        pad_nav_poll(input);
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { answer = 0; break; }
            SDL_Keycode k = ui_menu_key(&ev);
            int hv = ui_pointer_hover();
            if (hv == 0 || hv == 1) focus = hv;
            if (k == SDLK_LEFT)  focus = 0;
            if (k == SDLK_RIGHT) focus = 1;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { answer = focus == 0; break; }
            if (ui_is_back_key(k)) { answer = 0; break; }
        }
        ui_hit_begin();

        /* dim the (still running) video, then the dialog */
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        set_color(r, 0x00, 0x00, 0x00, 0xa0);
        fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

        const int BW = 900, BH = 300, BX = (SCREEN_W - BW) / 2, BY = (SCREEN_H - BH) / 2;
        set_color(r, COL_PANEL);
        fill_rounded_rect(r, BX, BY, BW, BH, 24);
        set_color(r, COL_BORDER);
        stroke_rounded_rect(r, BX, BY, BW, BH, 24);
        ui_text_draw(r, SCREEN_W / 2, BY + 50, "Put the console in rest mode?", 44, UI_FONT_BOLD,
                     C(COL_TEXT), UI_ALIGN_CENTER);

        const int W = 340, H = 104, GAP = 40, Y = BY + BH - H - 44;
        int x0 = SCREEN_W / 2 - W - GAP / 2;
        big_button(r, x0, Y, W, H, "Rest mode", focus == 0, true, false);
        big_button(r, x0 + W + GAP, Y, W, H, "Keep on", focus == 1, true, false);
        ui_hit_add(0, x0, Y, W, H);
        ui_hit_add(1, x0 + W + GAP, Y, W, H);

        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    while (SDL_PollEvent(&ev)) { }
    app_log_always("[UI] rest mode prompt: %s\n", answer ? "rest mode" : "keep on");
    return answer == 1;
}
