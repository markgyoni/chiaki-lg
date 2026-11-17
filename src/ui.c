#include "ui.h"
#include "config.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>

#include "config_import.h"
#include "ui_text.h"
#include "input.h"
#include "ui_padtest.h"
#include "app_log.h"

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

static void pad_nav_poll(struct InputContext *in)
{
    static uint32_t prev;
    static Uint32 repeat_at;
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
    prev = b;
}

/* ── Layout ──────────────────────────────────────────────────────────────────
 * Two-panel layout:
 *   Header band (full width, top)
 *   Left  panel — step guide + instructions
 *   Right panel — IP input + action buttons / keypad
 *   Status row  — config state pills
 *   Footer bar  — navigation hints
 */

#define SCREEN_W    1920
#define SCREEN_H    1080

/* Header */
#define HDR_Y       24
#define HDR_H       72

/* Content panels (below header) */
#define CONTENT_Y   (HDR_Y + HDR_H + 20)   /* 116 */
#define CONTENT_H   724

#define LEFT_X      60
#define LEFT_W      680
#define DIVIDER_X   (LEFT_X + LEFT_W + 12)
#define RIGHT_X     (DIVIDER_X + 12)
#define RIGHT_W     (SCREEN_W - RIGHT_X - 60)

/* Footer */
#define FOOTER_Y    (CONTENT_Y + CONTENT_H + 22)   /* 862 */
#define FOOTER_H    62

/* webOS remote keys we can actually use inside this native SDL UI.
 * BACK is often intercepted by webOS before SDL sees it on newer TVs,
 * so RED is offered as an in-app fallback "Return" key. */
#define WEBOS_KEY_BACK 1073742094
#define WEBOS_KEY_RED  403

/* ── Colour palette (R,G,B,A) ─────────────────────────────────────────────── */

/* Backgrounds */
#define COL_BG              0x04, 0x07, 0x12, 0xff   /* deep navy */
#define COL_BG_DEEPER       0x02, 0x04, 0x0c, 0xff   /* header overlay */
#define COL_PANEL           0x09, 0x11, 0x28, 0xff   /* panels / footer */
#define COL_ELEVATED        0x0d, 0x17, 0x32, 0xff   /* right panel */
#define COL_CARD            0x14, 0x20, 0x44, 0xff   /* button / input BG */
#define COL_CODE_BG         0x07, 0x0e, 0x22, 0xff   /* code block BG */

/* Accent */
#define COL_ACCENT          0x00, 0x72, 0xff, 0xff   /* PlayStation blue */
#define COL_ACCENT_MID      0x00, 0x48, 0xc0, 0xff
#define COL_ACCENT_DARK     0x00, 0x2e, 0x82, 0xff

/* Semantic */
#define COL_SUCCESS         0x00, 0xb8, 0x58, 0xff
#define COL_SUCCESS_BG      0x00, 0x28, 0x14, 0xff
#define COL_WARNING         0xf8, 0x9c, 0x1c, 0xff
#define COL_WARNING_BG      0x30, 0x1e, 0x04, 0xff
#define COL_ERROR           0xff, 0x44, 0x50, 0xff
#define COL_ERROR_BG        0x30, 0x08, 0x0c, 0xff

/* Borders */
#define COL_BORDER          0x1c, 0x2c, 0x58, 0xff
#define COL_BORDER_DIM      0x10, 0x18, 0x36, 0xff

/* Text */
#define COL_TEXT            0xec, 0xf2, 0xff, 0xff
#define COL_TEXT_DIM        0x78, 0x8c, 0xb8, 0xff
#define COL_TEXT_FAINT      0x3c, 0x4c, 0x78, 0xff
#define COL_TEXT_CODE       0x48, 0xcc, 0xff, 0xff

/* ── Primitive drawing helpers ───────────────────────────────────────────── */

static void set_color(SDL_Renderer *r, int R, int G, int B, int A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h)
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

static void draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale)
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

static int text_width(const char *text, int scale)
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
static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius)
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
static void stroke_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius)
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
static void hline(SDL_Renderer *r, int x, int y, int w)
{
    fill_rect(r, x, y, w, 1);
}

/* ── Button ─────────────────────────────────────────────────────────────── */

static void draw_button(SDL_Renderer *r, int x, int y, int w, int h,
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

static SDL_Color rgba(int R, int G, int B, int A) { SDL_Color c = { (Uint8)R, (Uint8)G, (Uint8)B, (Uint8)A }; return c; }
#define C(name) rgba(name)

/* Large button with TrueType label.  primary = accent-coloured when idle too. */
static void big_button(SDL_Renderer *r, int x, int y, int w, int h, const char *label,
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

static void render_setup_screen(SDL_Renderer *r, const char *status,
                                const char *ip, int focus,
                                const char *msg,
                                bool has_keys,
                                bool can_import,
                                bool can_connect,
                                bool show_keypad,
                                struct InputContext *input)
{
    (void)status;
    ui_text_init(r);
    const int M = 64, TOP = 150, LW = 900, RX = M + LW + 64, RW = SCREEN_W - RX - M;
    const int FOOT_Y = SCREEN_H - 76;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);

    /* ── Header ── */
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, 0, SCREEN_W, 6);
    ui_text_draw(r, M, 28, "Chiaki-lg", 60, UI_FONT_BOLD, C(COL_TEXT), UI_ALIGN_LEFT);
    ui_text_draw(r, M + ui_text_width("Chiaki-lg", 60, UI_FONT_BOLD) + 28, 50,
                 "PS5 Remote Play for LG TVs", 32, UI_FONT_REGULAR, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
    {
        const char *st = can_connect ? "Ready to connect" : "Setup needed";
        int tw = ui_text_width(st, 30, UI_FONT_SEMIBOLD);
        int cx = SCREEN_W - M - tw - 40;
        if (can_connect) set_color(r, COL_SUCCESS_BG); else set_color(r, COL_WARNING_BG);
        fill_rounded_rect(r, cx, 30, tw + 40, 54, 27);
        ui_text_draw(r, cx + 20, 30 + (54 - ui_text_line_height(30)) / 2, st, 30, UI_FONT_SEMIBOLD,
                     can_connect ? C(COL_SUCCESS) : C(COL_WARNING), UI_ALIGN_LEFT);
        ui_text_draw(r, SCREEN_W - M, 94, "v" APP_VERSION " (" APP_GIT ")", 24, UI_FONT_REGULAR,
                     C(COL_TEXT_FAINT), UI_ALIGN_RIGHT);
    }

    /* ── Left: address + actions ── */
    int y = TOP;
    ui_text_draw(r, M, y, "PS5 address", 30, UI_FONT_SEMIBOLD, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
    y += 46;
    {
        bool f = focus == 0 || show_keypad;
        if (f) { set_color(r, COL_ACCENT); fill_rounded_rect(r, M - 4, y - 4, LW + 8, 112, 16); }
        set_color(r, COL_CODE_BG);
        fill_rounded_rect(r, M, y, LW, 104, 14);
        const char *shown = (ip && ip[0]) ? ip : "192.168.x.x";
        ui_text_draw(r, M + 32, y + (104 - ui_text_line_height(52)) / 2, shown, 52, UI_FONT_SEMIBOLD,
                     (ip && ip[0]) ? C(COL_TEXT) : C(COL_TEXT_FAINT), UI_ALIGN_LEFT);
        if (f && ip && (SDL_GetTicks() / 500) % 2 == 0) {
            int cx = M + 32 + ui_text_width(ip, 52, UI_FONT_SEMIBOLD) + 4;
            set_color(r, COL_TEXT);
            fill_rect(r, cx, y + 26, 3, 52);
        }
        if (!show_keypad && f)
            ui_text_draw(r, M + LW - 28, y + (104 - ui_text_line_height(26)) / 2, "OK to edit", 26,
                         UI_FONT_REGULAR, C(COL_TEXT_DIM), UI_ALIGN_RIGHT);
    }
    y += 104 + 40;

    if (show_keypad) {
        static const char *KEYS[12] = { "1","2","3","4","5","6","7","8","9",".","0","Delete" };
        const int KW = 280, KH = 92, KG = 20;
        for (int i = 0; i < 12; i++) {
            int kx = M + (i % 3) * (KW + KG), ky = y + (i / 3) * (KH + KG);
            big_button(r, kx, ky, KW, KH, KEYS[i], focus == i + 1, true, false);
        }
        big_button(r, M, y + 4 * (KH + KG), 3 * KW + 2 * KG, KH, "Done", focus == 15, true, true);
    } else {
        const int BW = (LW - 24) / 2, BH = 104;
        big_button(r, M, y, BW, BH, "Import keys", focus == 13, can_import, false);
        big_button(r, M + BW + 24, y, BW, BH, "Connect", focus == 14, can_connect, true);
        y += BH + 24;
        big_button(r, M, y, BW, BH, "Settings", focus == 16, true, false);
        big_button(r, M + BW + 24, y, BW, BH, "Controller test", focus == 17, true, false);
        y += BH + 36;
        if (msg && msg[0]) {
            SDL_Color mc = C(COL_WARNING);
            if (strstr(msg, "OK") || strstr(msg, "updated")) mc = C(COL_SUCCESS);
            else if (strstr(msg, "ailed") || strstr(msg, "issing") || strstr(msg, "not found")) mc = C(COL_ERROR);
            ui_text_draw_wrapped(r, M, y, LW, msg, 30, UI_FONT_SEMIBOLD, mc, 3);
        }
    }

    /* ── Right: help for the current step + controllers ── */
    set_color(r, COL_PANEL);
    fill_rounded_rect(r, RX, TOP, RW, FOOT_Y - TOP - 30, 18);
    int ry = TOP + 30, rx = RX + 40, rw = RW - 80;
    SDL_Color tx = C(COL_TEXT), dim = C(COL_TEXT_DIM);
    if (!has_keys) {
        ui_text_draw(r, rx, ry, "Get started", 42, UI_FONT_BOLD, tx, UI_ALIGN_LEFT);
        ry += 66;
        ry += ui_text_draw_wrapped(r, rx, ry, rw,
            "1. On your PC, register your PS5 in chiaki-ng, then use Settings > Config > "
            "Export Settings To File to save chiaki-ng-Default.ini.", 30, UI_FONT_REGULAR, tx, 0) + 16;
        ry += ui_text_draw_wrapped(r, rx, ry, rw,
            "2. Copy that file to the TV (webOS Dev Manager or adb push) into:", 30,
            UI_FONT_REGULAR, tx, 0) + 8;
        ry += ui_text_draw_wrapped(r, rx + 20, ry, rw - 20,
            "/media/developer/apps/usr/palm/applications/org.homebrew.chiaki/", 26,
            UI_FONT_REGULAR, C(COL_TEXT_CODE), 0) + 16;
        ry += ui_text_draw_wrapped(r, rx, ry, rw,
            "3. Enter the PS5 address on the left and press Import keys.", 30,
            UI_FONT_REGULAR, tx, 0) + 24;
    } else if (!can_connect) {
        ui_text_draw(r, rx, ry, "Enter the PS5 address", 42, UI_FONT_BOLD, tx, UI_ALIGN_LEFT);
        ry += 66;
        ry += ui_text_draw_wrapped(r, rx, ry, rw,
            "On the PS5: Settings > Network > Connection Status > View Connection Status. "
            "Use the IPv4 address, for example 192.168.1.50.", 30, UI_FONT_REGULAR, tx, 0) + 24;
    } else {
        ui_text_draw(r, rx, ry, "Ready to play", 42, UI_FONT_BOLD, tx, UI_ALIGN_LEFT);
        ry += 66;
        ry += ui_text_draw_wrapped(r, rx, ry, rw,
            "Press Connect. During the stream, press Up on the remote for stream statistics, "
            "Down twice to return here, and Back to close the app.", 30, UI_FONT_REGULAR, dim, 0) + 24;
    }

    /* controllers (live) */
    set_color(r, COL_BORDER_DIM);
    fill_rect(r, rx, ry, rw, 2);
    ry += 24;
    ui_text_draw(r, rx, ry, "Controllers", 34, UI_FONT_BOLD, tx, UI_ALIGN_LEFT);
    ry += 54;
    InputPadInfo pads[4];
    int n = input ? input_get_pads(input, pads, 4) : 0;
    if (n == 0)
        ui_text_draw_wrapped(r, rx, ry, rw, "None connected. Turn a controller on, or plug it in by USB.",
                             28, UI_FONT_REGULAR, C(COL_WARNING), 2);
    for (int i = 0; i < n && i < 3 && ry < FOOT_Y - 140; i++) {
        if (pads[i].outputs_active) set_color(r, COL_SUCCESS); else set_color(r, COL_WARNING);
        fill_rounded_rect(r, rx, ry + 12, 14, 14, 7);
        ui_text_draw_fit(r, rx + 30, ry, rw - 30, pads[i].name, 30, UI_FONT_SEMIBOLD, tx);
        ry += 40;
        char line[160];
        snprintf(line, sizeof(line), "%s  -  %s", pads[i].connection, pads[i].outputs);
        ry += ui_text_draw_wrapped(r, rx + 30, ry, rw - 30, line, 26, UI_FONT_REGULAR, dim, 2) + 14;
    }

    /* ── Footer ── */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, FOOT_Y, SCREEN_W, SCREEN_H - FOOT_Y);
    ui_text_draw(r, SCREEN_W / 2, FOOT_Y + (SCREEN_H - FOOT_Y - ui_text_line_height(28)) / 2,
                 show_keypad ? "Arrows: move    OK: type    Back: close keypad"
                             : "Arrows / D-pad: move    OK / Cross: select    Back / Circle: exit",
                 28, UI_FONT_REGULAR, dim, UI_ALIGN_CENTER);
}


/* ══════════════════════════════════════════════════════════════════════════
 * Settings screen
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    /* left column */
    SET_RESOLUTION = 0,   /* VIDEO */
    SET_FPS,
    SET_BITRATE,
    SET_CODEC,
    SET_PS5,              /* CONNECTION */
    SET_WAKE_ON_START,
    SET_SLEEP_ON_EXIT,
    SET_LOG_LEVEL,        /* ADVANCED */
    SET_INPUT_DEBUG,
    /* right column */
    SET_PROFILE,          /* CONTROLLERS */
    SET_RUMBLE,
    SET_DS_HID,
    SET_MOTION,
    SET_TRIGGERS,
    SET_BT_PATCH,
    SET_HD_HAPTICS,       /* DualSense USB audio */
    SET_MIC,
    SET_RETURN,
    SET_COUNT
} SettingsFocus;

#define SET_FIRST_RIGHT SET_PROFILE

static const char *PROFILE_OPTIONS[] = {"auto", "sony_generic", "sony_kernel", "xbox_bt", "xpad", "generic"};
static const char *PROFILE_LABELS[]  = {"Automatic", "PlayStation (HID)", "PlayStation (kernel)",
                                        "Xbox (Bluetooth)", "Xbox (USB)", "Generic"};
#define N_PROFILES ((int)(sizeof(PROFILE_OPTIONS) / sizeof(PROFILE_OPTIONS[0])))

/* One line of help per setting, shown under the list for the focused row. */
static const char *SETTING_HELP[SET_COUNT] = {
    [SET_RESOLUTION]    = "Stream resolution. The PS5 streams Remote Play at up to 1080p; the TV upscales it.",
    [SET_FPS]           = "60 fps feels much smoother; 30 fps uses half the bandwidth.",
    [SET_BITRATE]       = "Higher looks sharper but needs a stronger network. Lower it if the picture stutters.",
    [SET_CODEC]         = "H.265 gives better quality per bit. Use H.265 HDR for HDR games on an HDR TV.",
    [SET_PS5]           = "On for a PS5, off for a PS4 (PS4 streams always use H.264).",
    [SET_WAKE_ON_START] = "Wake the console from rest mode when you press Connect.",
    [SET_SLEEP_ON_EXIT] = "Put the console into rest mode when you close the app.",
    [SET_LOG_LEVEL]     = "How much is written to /tmp/chiaki.log. Warning is enough unless you are debugging.",
    [SET_INPUT_DEBUG]   = "Log every raw controller event. Only for diagnosing controller problems.",
    [SET_PROFILE]       = "Button layout. Automatic picks the right one for each controller.",
    [SET_RUMBLE]        = "Pass vibration from games to your controller.",
    [SET_DS_HID]        = "Read the DualSense directly: touchpad, motion, adaptive triggers, lightbar.",
    [SET_MOTION]        = "Send DualSense gyro and accelerometer to the console (motion aiming).",
    [SET_TRIGGERS]      = "DualSense adaptive trigger resistance from games.",
    [SET_BT_PATCH]      = "Rooted TVs: fix the TV's Bluetooth so DualSense vibration, triggers and lightbar "
                          "work wirelessly. Nothing is changed on disk; a reboot undoes it.",
    [SET_HD_HAPTICS]    = "DualSense on USB: play the game's haptics on the controller's actuators (fine-grained, like on a PS5) instead of plain rumble.",
    [SET_MIC]           = "DualSense on USB: use its microphone for voice chat. Press the controller's mute button during a stream to switch it on or off.",
    [SET_RETURN]        = "Go back to the start screen.",
};

typedef struct { int w, h; const char *label; } ResOption;

static const ResOption RES_OPTIONS[] = {
    {1280, 720,  "720p"},
    {1920, 1080, "1080p"},
    /* No 1440p/2160p: the PS5 streams Remote Play at up to 1080p and refuses
     * larger sizes ("AvCap failed to initialize video"). */
};
static const int FPS_OPTIONS[]    = {30, 60};
static const int BITRATE_OPTIONS[] = {5000, 10000, 15000, 25000, 35000, 45000};
static const char *CODEC_OPTIONS[] = {"h264", "h265", "h265_hdr"};
static const char *CODEC_LABELS[]  = {"H.264 (AVC)", "H.265 (HEVC)", "H.265 HDR"};
static const char *LOG_LEVEL_STRS[]   = {"off","error","warning","info","verbose","debug"};
static const char *LOG_LEVEL_LABELS[] = {"Off","Error","Warning","Info","Verbose","Debug"};

static const char *ui_log_level_to_string(int mask)
{
    if (mask == 0) return "off";
    if (mask == 16) return "error";
    if ((mask & (16|8|4|2|1)) == (16|8|4|2|1)) return "debug";
    if ((mask & (16|8|4|2))   == (16|8|4|2))   return "verbose";
    if ((mask & (16|8|4))     == (16|8|4))     return "info";
    if ((mask & (16|8))       == (16|8))       return "warning";
    return "warning";
}

static int ui_log_level_from_string(const char *s)
{
    if (!s || !s[0] || !strcmp(s,"off"))     return 0;
    if (!strcmp(s,"error"))                  return 16;
    if (!strcmp(s,"warning"))                return 16|8;
    if (!strcmp(s,"info"))                   return 16|8|4;
    if (!strcmp(s,"verbose"))                return 16|8|4|2;
    if (!strcmp(s,"debug"))                  return 16|8|4|2|1;
    return 16|8;
}

static int ui_find_resolution_idx(const AppConfig *cfg)
{
    int w = cfg ? cfg->video_width : 1920, h = cfg ? cfg->video_height : 1080;
    for (int i = 0; i < (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0])); i++)
        if (RES_OPTIONS[i].w == w && RES_OPTIONS[i].h == h) return i;
    return 1;
}
static int ui_find_fps_idx(const AppConfig *cfg)
{
    int fps = cfg ? cfg->video_fps : 60;
    for (int i = 0; i < (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0])); i++)
        if (FPS_OPTIONS[i] == fps) return i;
    return 1;
}
static int ui_find_bitrate_idx(const AppConfig *cfg)
{
    int br = cfg ? cfg->video_bitrate : 15000;
    for (int i = 0; i < (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); i++)
        if (BITRATE_OPTIONS[i] == br) return i;
    int best = 2, best_diff = 1000000;
    for (int i = 0; i < (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); i++) {
        int d = BITRATE_OPTIONS[i] - br; if (d < 0) d = -d;
        if (d < best_diff) { best_diff = d; best = i; }
    }
    return best;
}
static int ui_find_codec_idx(const AppConfig *cfg)
{
    const char *c = (cfg && cfg->video_codec) ? cfg->video_codec : "h265";
    for (int i = 0; i < 3; i++) if (!strcmp(CODEC_OPTIONS[i], c)) return i;
    return 1;
}
static int ui_find_log_level_idx(const AppConfig *cfg)
{
    const char *s = ui_log_level_to_string(cfg ? cfg->log_level : (16|8));
    for (int i = 0; i < 6; i++) if (!strcmp(LOG_LEVEL_STRS[i], s)) return i;
    return 2;
}

/* Draw a settings option row with label on left, value chip on right */
static void ui_draw_option_row(SDL_Renderer *r, int x, int y, int w, int h,
                               const char *label, const char *value, bool focused)
{
    int radius = 8;

    if (focused) {
        /* Focused row: accent fill */
        set_color(r, COL_ACCENT_DARK);
        fill_rounded_rect(r, x, y, w, h, radius);
        /* Left focus indicator */
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
        /* Value chip */
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

static const char *on_off(bool v) { return v ? "On" : "Off"; }

static void ui_render_settings_screen(SDL_Renderer *r,
                                      const AppConfig *cfg,
                                      int focus,
                                      const char *msg,
                                      int res_idx, int fps_idx,
                                      int br_idx,  int codec_idx,
                                      int log_idx, int profile_idx)
{
    /* Background + header */
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
    set_color(r, COL_PANEL);
    fill_rect(r, 0, HDR_Y, SCREEN_W, HDR_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, HDR_Y + HDR_H - 1, SCREEN_W);
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, HDR_Y, 8, HDR_H);
    set_color(r, COL_TEXT);
    draw_text(r, 36, HDR_Y + 18, "Settings", 4);
    set_color(r, COL_TEXT_DIM);
    draw_text(r, SCREEN_W - 48 - text_width("Saved automatically", 2), HDR_Y + 30,
              "Saved automatically", 2);

    /* Two columns of rows */
    const int ROW_H = 58, ROW_GAP = 8, GROUP_GAP = 18;
    const int COL_W = (SCREEN_W - 60 * 2 - 40) / 2;
    const int LX = 60, RX = LX + COL_W + 40;
    char vbuf[64];

#define GROUP(x, y, name) do { set_color(r, COL_ACCENT); draw_text(r, (x) + 4, (y), name, 2); (y) += 34; } while (0)
#define ROW(x, y, id, label, value) do { \
        ui_draw_option_row(r, (x), (y), COL_W, ROW_H, label, value, focus == (id)); \
        (y) += ROW_H + ROW_GAP; } while (0)

    int y = CONTENT_Y + 6;
    GROUP(LX, y, "VIDEO");
    snprintf(vbuf, sizeof(vbuf), "%s  (%dx%d)",
             RES_OPTIONS[res_idx].label, RES_OPTIONS[res_idx].w, RES_OPTIONS[res_idx].h);
    ROW(LX, y, SET_RESOLUTION, "Resolution", vbuf);
    snprintf(vbuf, sizeof(vbuf), "%d fps", FPS_OPTIONS[fps_idx]);
    ROW(LX, y, SET_FPS, "Frame rate", vbuf);
    snprintf(vbuf, sizeof(vbuf), "%d Mbit/s", BITRATE_OPTIONS[br_idx] / 1000);
    ROW(LX, y, SET_BITRATE, "Bitrate", vbuf);
    ROW(LX, y, SET_CODEC, "Codec", CODEC_LABELS[codec_idx]);
    y += GROUP_GAP;
    GROUP(LX, y, "CONNECTION");
    ROW(LX, y, SET_PS5, "PS5 console", on_off(cfg && cfg->ps5));
    ROW(LX, y, SET_WAKE_ON_START, "Wake console on connect", on_off(cfg && cfg->wakeup));
    ROW(LX, y, SET_SLEEP_ON_EXIT, "Rest mode on exit", on_off(cfg && cfg->sleep_on_exit));
    y += GROUP_GAP;
    GROUP(LX, y, "ADVANCED");
    ROW(LX, y, SET_LOG_LEVEL, "Log level", LOG_LEVEL_LABELS[log_idx]);
    ROW(LX, y, SET_INPUT_DEBUG, "Controller debug log", on_off(cfg && cfg->input_debug));

    y = CONTENT_Y + 6;
    GROUP(RX, y, "CONTROLLERS");
    ROW(RX, y, SET_PROFILE, "Button layout", PROFILE_LABELS[profile_idx]);
    ROW(RX, y, SET_RUMBLE, "Vibration", on_off(cfg && cfg->rumble));
    ROW(RX, y, SET_DS_HID, "DualSense enhanced", on_off(cfg && cfg->dualsense_hid));
    ROW(RX, y, SET_MOTION, "Motion controls", on_off(cfg && cfg->motion));
    ROW(RX, y, SET_TRIGGERS, "Adaptive triggers", on_off(cfg && cfg->trigger_effects));
    ROW(RX, y, SET_BT_PATCH, "Bluetooth fix (DualSense)", on_off(cfg && cfg->dualsense_bt_patch));
    ROW(RX, y, SET_HD_HAPTICS, "HD haptics (USB)", on_off(cfg && cfg->dualsense_hd_haptics));
    ROW(RX, y, SET_MIC, "Microphone (USB)", on_off(cfg && cfg->dualsense_mic));
    y += GROUP_GAP;
    draw_button(r, RX, y, COL_W, ROW_H + 6, "Back", focus == SET_RETURN, true);
#undef ROW
#undef GROUP

    /* Help for the focused row */
    const int FOOT_Y = SCREEN_H - 70, FOOT_H = 70;
    int help_y = FOOT_Y - 116;
    set_color(r, COL_PANEL);
    fill_rounded_rect(r, LX, help_y, SCREEN_W - 2 * LX, 100, 12);
    set_color(r, COL_ACCENT);
    fill_rect(r, LX, help_y + 10, 4, 80);
    if (focus >= 0 && focus < SET_COUNT && SETTING_HELP[focus]) {
        SDL_Color c = { 0xec, 0xf2, 0xff, 0xff };
        ui_text_draw_wrapped(r, LX + 28, help_y + 14, SCREEN_W - 2 * LX - 56, SETTING_HELP[focus],
                             30, UI_FONT_REGULAR, c, 2);
    }

    /* Footer */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, FOOT_Y, SCREEN_W, FOOT_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, FOOT_Y, SCREEN_W);
    set_color(r, COL_TEXT_DIM);
    draw_text(r, 48, FOOT_Y + (FOOT_H - 14) / 2,
              "Up/Down: move   Left/Right/OK: change   Back: return", 2);
    if (msg && msg[0]) {
        set_color(r, COL_SUCCESS);
        draw_text(r, SCREEN_W - 48 - text_width(msg, 2), FOOT_Y + (FOOT_H - 14) / 2, msg, 2);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Loading screen
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_loading(SDL_Renderer *r, const char *base_text)
{
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
    draw_text(r, 40, 30, "CHIAKI", 3);

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

void ui_render_stats_overlay(SDL_Renderer *r, const char *text)
{
    if (!r || !text || !text[0]) return;

    int sw = SCREEN_W, sh = SCREEN_H;
    ui_screen_size(r, &sw, &sh);

    const int scale  = 2;
    const int pad    = 20;
    ui_text_init(r);
    const int line_h = ui_text_line_height(scale_px(scale));

    /* Measure (text_width measures up to the first '\n') */
    int lines = 0, maxw = 0;
    for (const char *p = text; p; ) {
        int w = text_width(p, scale);
        if (w > maxw) maxw = w;
        lines++;
        p = strchr(p, '\n');
        if (p) p++;
    }

    int box_w = maxw + pad * 2 + 8;  /* +8 for left accent bar */
    int box_h = lines * line_h + pad * 2;
    if (box_w > sw - 40) box_w = sw - 40;
    if (box_h > sh - 40) box_h = sh - 40;

    int x = 24, y = 24;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Shadow */
    set_color(r, 0x00, 0x00, 0x00, 100);
    fill_rounded_rect(r, x + 4, y + 4, box_w, box_h, 12);

    /* Background */
    set_color(r, 0x06, 0x0e, 0x24, 210);
    fill_rounded_rect(r, x, y, box_w, box_h, 12);

    /* Border */
    set_color(r, COL_BORDER);
    stroke_rounded_rect(r, x, y, box_w, box_h, 12);

    /* Left accent bar */
    set_color(r, COL_ACCENT);
    fill_rect(r, x, y + 3, 4, box_h - 6);

    /* Text */
    set_color(r, COL_TEXT);
    draw_text(r, x + pad + 8, y + pad, text, scale);

    /* Top label */
    set_color(r, COL_ACCENT_MID);
    hline(r, x + 4, y + pad + lines * line_h + 6, box_w - 8);
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

/* ══════════════════════════════════════════════════════════════════════════
 * Shared helpers (config / validation)
 * ══════════════════════════════════════════════════════════════════════════ */

static bool ui_has_keys(const AppConfig *cfg)
{
    return cfg
        && cfg->psn_account_id_b64 && cfg->psn_account_id_b64[0]
        && cfg->registered_key_b64 && cfg->registered_key_b64[0]
        && cfg->rp_key_b64 && cfg->rp_key_b64[0]
        && strlen(cfg->registered_key_b64) >= 8;
}

static bool ui_valid_ipv4(const char *s)
{
    if (!s || !s[0]) return false;
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static bool ui_write_host(const char *config_path, const char *host)
{
    if (!config_path || !host || !host[0]) return false;
    struct json_object *root = json_object_from_file(config_path);
    if (!root) root = json_object_new_object();
    json_object_object_del(root, "host");
    json_object_object_add(root, "host", json_object_new_string(host));
    int rc = json_object_to_file_ext(config_path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return rc == 0;
}

static bool ui_write_settings_json(const char *config_path, const AppConfig *cfg)
{
    if (!config_path || !cfg) return false;
    struct json_object *root = json_object_from_file(config_path);
    if (!root) root = json_object_new_object();

#define SET_INT(k, v)  json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_int(v))
#define SET_BOOL(k, v) json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_boolean(v))
#define SET_STR(k, v)  json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_string(v))

    SET_INT ("video_width",   cfg->video_width);
    SET_INT ("video_height",  cfg->video_height);
    SET_INT ("video_fps",     cfg->video_fps);
    SET_INT ("video_bitrate", cfg->video_bitrate);
    SET_BOOL("ps5",           cfg->ps5);
    SET_BOOL("hw_decode",     cfg->hw_decode);
    SET_BOOL("wakeup",        cfg->wakeup);
    SET_BOOL("sleep_on_exit", cfg->sleep_on_exit);
    SET_STR ("video_codec",   (cfg->video_codec && cfg->video_codec[0]) ? cfg->video_codec : "h265");
    SET_STR ("log_level",     ui_log_level_to_string(cfg->log_level));
    SET_STR ("controller_profile", (cfg->controller_profile && cfg->controller_profile[0]) ? cfg->controller_profile : "auto");
    SET_BOOL("rumble",             cfg->rumble);
    SET_BOOL("dualsense_hid",      cfg->dualsense_hid);
    SET_BOOL("motion",             cfg->motion);
    SET_BOOL("trigger_effects",    cfg->trigger_effects);
    SET_BOOL("dualsense_hd_haptics", cfg->dualsense_hd_haptics);
    SET_BOOL("dualsense_mic",      cfg->dualsense_mic);
    SET_BOOL("dualsense_bt_patch", cfg->dualsense_bt_patch);
    SET_BOOL("input_debug",        cfg->input_debug);

#undef SET_INT
#undef SET_BOOL
#undef SET_STR

    int rc = json_object_to_file_ext(config_path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return rc == 0;
}

static void ui_reload_cfg(AppConfig *cfg, const char *config_path)
{
    if (!cfg) return;
    config_free(cfg);
    (void)config_load(cfg, config_path);
}

static void ui_build_ini_path(const char *config_path, char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    out[0] = '\0';
    if (!config_path) { snprintf(out, outlen, "chiaki-ng-Default.ini"); return; }
    const char *slash = strrchr(config_path, '/');
    if (!slash) { snprintf(out, outlen, "chiaki-ng-Default.ini"); return; }
    size_t dirlen = (size_t)(slash - config_path);
    if (dirlen + 1 >= outlen) dirlen = outlen - 2;
    memcpy(out, config_path, dirlen); out[dirlen] = '\0';
    snprintf(out + dirlen, outlen - dirlen, "/chiaki-ng-Default.ini");
}

static void ui_set_codec(AppConfig *cfg, const char *codec)
{
    if (!cfg) return;
    if (!codec) codec = "h265";
    if (cfg->video_codec && !strcmp(cfg->video_codec, codec)) return;
    free(cfg->video_codec);
    cfg->video_codec = strdup(codec);
}

static void ui_apply_settings_indices(AppConfig *cfg,
                                      int res_idx, int fps_idx,
                                      int br_idx,  int codec_idx, int log_idx)
{
    if (!cfg) return;
    if (res_idx   < 0 || res_idx   >= (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0]))) res_idx   = 1;
    if (fps_idx   < 0 || fps_idx   >= (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0]))) fps_idx   = 1;
    if (br_idx    < 0 || br_idx    >= (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0]))) br_idx = 2;
    if (codec_idx < 0 || codec_idx >= 3) codec_idx = 1;
    if (log_idx   < 0 || log_idx   >= 6) log_idx   = 2;

    cfg->video_width   = RES_OPTIONS[res_idx].w;
    cfg->video_height  = RES_OPTIONS[res_idx].h;
    cfg->video_fps     = FPS_OPTIONS[fps_idx];
    cfg->video_bitrate = BITRATE_OPTIONS[br_idx];
    ui_set_codec(cfg, CODEC_OPTIONS[codec_idx]);
    cfg->log_level = ui_log_level_from_string(LOG_LEVEL_STRS[log_idx]);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Settings event loop
 * ══════════════════════════════════════════════════════════════════════════ */

static void ui_run_settings(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, struct InputContext *input)
{
    if (!renderer || !cfg) return;

    int res_idx   = ui_find_resolution_idx(cfg);
    int fps_idx   = ui_find_fps_idx(cfg);
    int br_idx    = ui_find_bitrate_idx(cfg);
    int codec_idx = ui_find_codec_idx(cfg);
    int log_idx   = ui_find_log_level_idx(cfg);
    int profile_idx = 0;
    for (int i = 0; i < N_PROFILES; i++)
        if (cfg->controller_profile && !strcmp(cfg->controller_profile, PROFILE_OPTIONS[i])) profile_idx = i;
    int focus     = SET_RESOLUTION;
    bool input_changed = false;
    char msg[256] = {0};

    bool wants_exit = false;

    while (1) {
        pad_nav_poll(input);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { wants_exit = true; continue; }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode sym = ev.key.keysym.sym;
                if (sym == SDLK_ESCAPE || sym == WEBOS_KEY_BACK || sym == WEBOS_KEY_RED) { wants_exit = true; continue; }

                if      (sym == SDLK_UP   && focus > 0)          focus--;
                else if (sym == SDLK_DOWN && focus < SET_RETURN) focus++;
                else {
                    bool fwd = (sym == SDLK_RIGHT || sym == SDLK_RETURN ||
                                sym == SDLK_KP_ENTER || sym == SDLK_SPACE);
                    bool changed = false;
                    int n;

                    if      (focus == SET_RESOLUTION)  { n = (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0])); res_idx   = (res_idx   + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_FPS)          { n = (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0])); fps_idx   = (fps_idx   + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_BITRATE)      { n = (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); br_idx = (br_idx + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_CODEC)        { codec_idx = (codec_idx + (fwd?1:-1) + 3) % 3; changed = true; }
                    else if (focus == SET_LOG_LEVEL)    { log_idx   = (log_idx   + (fwd?1:-1) + 6) % 6; changed = true; }
                    else if (focus == SET_PS5)          { cfg->ps5         = !cfg->ps5;         changed = true; }
                    else if (focus == SET_INPUT_DEBUG)  { cfg->input_debug = !cfg->input_debug; changed = input_changed = true; }
                    else if (focus == SET_PROFILE)      { profile_idx = (profile_idx + (fwd?1:-1) + N_PROFILES) % N_PROFILES;
                                                          free(cfg->controller_profile); cfg->controller_profile = strdup(PROFILE_OPTIONS[profile_idx]);
                                                          changed = input_changed = true; }
                    else if (focus == SET_RUMBLE)       { cfg->rumble = !cfg->rumble; changed = input_changed = true; }
                    else if (focus == SET_DS_HID)       { cfg->dualsense_hid = !cfg->dualsense_hid; changed = input_changed = true; }
                    else if (focus == SET_MOTION)       { cfg->motion = !cfg->motion; changed = input_changed = true; }
                    else if (focus == SET_TRIGGERS)     { cfg->trigger_effects = !cfg->trigger_effects; changed = input_changed = true; }
                    else if (focus == SET_BT_PATCH)     { cfg->dualsense_bt_patch = !cfg->dualsense_bt_patch; changed = input_changed = true; }
                    else if (focus == SET_HD_HAPTICS)   { cfg->dualsense_hd_haptics = !cfg->dualsense_hd_haptics; changed = true; }
                    else if (focus == SET_MIC)          { cfg->dualsense_mic = !cfg->dualsense_mic; changed = true; }
                    else if (focus == SET_WAKE_ON_START){ cfg->wakeup      = !cfg->wakeup;      changed = true; }
                    else if (focus == SET_SLEEP_ON_EXIT){ cfg->sleep_on_exit = !cfg->sleep_on_exit; changed = true; }
                    else if (focus == SET_RETURN)       { wants_exit = true; continue; }

                    if (changed) {
                        ui_apply_settings_indices(cfg, res_idx, fps_idx, br_idx, codec_idx, log_idx);
                        if (!ui_write_settings_json(config_path, cfg))
                            snprintf(msg, sizeof(msg), "Failed to write config.json");
                        else
                            snprintf(msg, sizeof(msg), "Saved.");
                    }
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) { wants_exit = true; continue; }

                SDL_Keycode fake_sym = 0;
                if      (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    fake_sym = SDLK_UP;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  fake_sym = SDLK_DOWN;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  fake_sym = SDLK_LEFT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) fake_sym = SDLK_RIGHT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)          fake_sym = SDLK_RETURN;

                if (fake_sym) {
                    SDL_Event fake; memset(&fake, 0, sizeof(fake));
                    fake.type = SDL_KEYDOWN; fake.key.keysym.sym = fake_sym;
                    SDL_PushEvent(&fake);
                }
            }
        }

        /* If Back/B/Return was pressed, finish draining the current event
         * batch, wait for any companion events, drain again, then return
         * cleanly to the launcher. */
        if (wants_exit) {
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
            { SDL_Event _d; while (SDL_PollEvent(&_d)) {} }
            return;
        }

        ui_render_settings_screen(renderer, cfg, focus, msg,
                                   res_idx, fps_idx, br_idx, codec_idx, log_idx, profile_idx);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main registration / launcher UI
 * ══════════════════════════════════════════════════════════════════════════ */

static char g_notice[256];

void ui_set_launcher_notice(const char *text)
{
    snprintf(g_notice, sizeof(g_notice), "%s", text ? text : "");
}

UIResult ui_run_registration(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, ChiakiLog *log,
                             struct InputContext *input)
{
    (void)log;

    /* Pre-fill IP from config */
    char ip[64] = {0};
    if (cfg && cfg->host && cfg->host[0])
        snprintf(ip, sizeof(ip), "%s", cfg->host);

    /* Focus mapping:
     *   Main page:  0=IP field, 13=Import, 14=Connect, 16=Settings
     *   Keypad:     1..12=digit keys, 15=Done
     */
    int  focus   = 0;
    bool editing = false;
    char msg[256] = {0};
    if (g_notice[0]) {
        snprintf(msg, sizeof(msg), "%s", g_notice);
        g_notice[0] = '\0';
    }

    /* Start with Connect focused if already ready */
    if (ui_valid_ipv4(ip) && ui_has_keys(cfg))
        focus = 14;

    /* Once per process: on webOS a second SDL_StartTextInput (launcher shown
     * again after a stream) crashes in wl_proxy_marshal. */
    static bool text_input_started;
    if (!text_input_started) {
        SDL_StartTextInput();
        text_input_started = true;
    }

    while (1) {
        pad_nav_poll(input);
        /* Build lightweight debug status string */
        char status[256];
        snprintf(status, sizeof(status),
            "host:%s  acct:%s  rk:%s  rpk:%s  rt:%s",
            (cfg && cfg->host) ? cfg->host : "(null)",
            (cfg && cfg->psn_account_id_b64) ? "OK" : "NO",
            (cfg && cfg->registered_key_b64 && strlen(cfg->registered_key_b64) >= 8) ? "OK" : "NO",
            (cfg && cfg->rp_key_b64) ? "OK" : "NO",
            (cfg && cfg->psn_refresh_token) ? "OK" : "none");

        bool has_keys   = ui_has_keys(cfg);
        bool can_import = ui_valid_ipv4(ip);
        bool can_connect= ui_valid_ipv4(ip) && has_keys;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return UI_RESULT_QUIT;

            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode sym = ev.key.keysym.sym;

                /* Back key: close keypad if open, otherwise exit launcher */
                if (sym == SDLK_AC_HOME) return UI_RESULT_QUIT;
                if (sym == SDLK_ESCAPE || sym == WEBOS_KEY_BACK || sym == WEBOS_KEY_RED) {
                    if (editing) { editing = false; focus = 0; continue; }
                    return UI_RESULT_QUIT;
                }

                /* Navigation */
                if (sym == SDLK_UP) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int row = (focus - 1) / 3;
                            if (row > 0) focus -= 3;
                        } else if (focus == 15) {
                            focus = 10; /* "0" key */
                        }
                    } else {
                        if (focus == 13 || focus == 14) focus = 0;
                        else if (focus == 16)            focus = 13;
                        else if (focus == 17)            focus = 14;
                    }
                } else if (sym == SDLK_DOWN) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int row = (focus - 1) / 3;
                            if (row == 3) focus = 15;
                            else          focus += 3;
                        }
                    } else {
                        if (focus == 0)               focus = 13;
                        else if (focus == 13)            focus = 16;
                        else if (focus == 14)            focus = 17;
                    }
                } else if (sym == SDLK_LEFT) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int col = (focus - 1) % 3;
                            if (col > 0) focus--;
                        }
                    } else {
                        if (focus == 14) focus = 13;
                        else if (focus == 17) focus = 16;
                    }
                } else if (sym == SDLK_RIGHT) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int col = (focus - 1) % 3;
                            if (col < 2) focus++;
                        }
                    } else {
                        if (focus == 13) focus = 14;
                        else if (focus == 16) focus = 17;
                    }
                }

                /* Physical keyboard IP entry */
                if (focus == 0 || editing) {
                    size_t len = strlen(ip);
                    if (sym == SDLK_BACKSPACE) {
                        if (len > 0) ip[len - 1] = '\0';
                    } else if ((sym >= SDLK_0 && sym <= SDLK_9) ||
                               (sym >= SDLK_KP_0 && sym <= SDLK_KP_9) ||
                               sym == SDLK_PERIOD || sym == SDLK_KP_PERIOD) {
                        if (len + 1 < sizeof(ip)) {
                            char c;
                            if      (sym == SDLK_PERIOD || sym == SDLK_KP_PERIOD) c = '.';
                            else if (sym >= SDLK_0 && sym <= SDLK_9)              c = '0' + (char)(sym - SDLK_0);
                            else                                                   c = '0' + (char)(sym - SDLK_KP_0);
                            ip[len] = c; ip[len + 1] = '\0';
                        }
                    }
                }

                /* Select / Activate */
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
                    if (editing && focus >= 1 && focus <= 12) {
                        /* Keypad digit press */
                        static const char *K[12] = {
                            "1","2","3","4","5","6","7","8","9",".","0","DEL"
                        };
                        const char *k = K[focus - 1];
                        size_t len = strlen(ip);
                        if (!strcmp(k, "DEL")) {
                            if (len > 0) ip[len - 1] = '\0';
                        } else if (len + 1 < sizeof(ip)) {
                            char c = k[0];
                            if (c == '.' || (c >= '0' && c <= '9'))
                                { ip[len] = c; ip[len + 1] = '\0'; }
                        }
                    } else if (editing && focus == 15) {
                        editing = false; focus = 0;
                    } else if (!editing && focus == 0) {
                        editing = true; focus = 1;
                    } else if (!editing && focus == 13) {
                        /* Import Config */
                        if (!ui_valid_ipv4(ip)) {
                            snprintf(msg, sizeof(msg), "Enter a valid PS5 IP address first.");
                            focus = 0; continue;
                        }
                        if (!ui_write_host(config_path, ip)) {
                            snprintf(msg, sizeof(msg), "Failed to write host to config.json");
                            continue;
                        }
                        char ini_path[512];
                        ui_build_ini_path(config_path, ini_path, sizeof(ini_path));
                        ChiakiImportResult res = config_try_import_chiaki_ini(ini_path, config_path);
                        switch (res) {
                        case CI_FILE_NOT_FOUND:
                            snprintf(msg, sizeof(msg), "chiaki-ng-Default.ini not found. Copy it to the TV first.");
                            break;
                        case CI_PARSE_ERROR:
                            snprintf(msg, sizeof(msg), "Import failed: could not parse chiaki-ng INI.");
                            break;
                        case CI_WRITE_ERROR:
                            snprintf(msg, sizeof(msg), "Import failed: could not write config.json.");
                            break;
                        case CI_SUCCESS:
                        case CI_SUCCESS_NEEDS_HOST:
                            snprintf(msg, sizeof(msg), "Import OK.  Press Connect.");
                            ui_reload_cfg(cfg, config_path);
                            if (cfg && cfg->host && cfg->host[0])
                                snprintf(ip, sizeof(ip), "%s", cfg->host);
                            if (ui_has_keys(cfg) && ui_valid_ipv4(ip))
                                focus = 14;
                            break;
                        default:
                            snprintf(msg, sizeof(msg), "Import complete.");
                            ui_reload_cfg(cfg, config_path);
                            break;
                        }
                    } else if (!editing && focus == 17) {
                        ui_run_padtest(renderer, input);
                        msg[0] = '\0';
                    } else if (!editing && focus == 16) {
                        /* Settings — drains Back events internally before returning */
                        ui_run_settings(renderer, cfg, config_path, input);
                        ui_reload_cfg(cfg, config_path);
                        if (cfg && cfg->host && cfg->host[0])
                            snprintf(ip, sizeof(ip), "%s", cfg->host);
                        snprintf(msg, sizeof(msg), "Settings updated.");
                        /* Keep focus on Settings: jumping to Connect made a
                         * second OK press start a session unexpectedly. */
                    } else if (!editing && focus == 14) {
                        /* Connect */
                        if (!ui_valid_ipv4(ip)) {
                            snprintf(msg, sizeof(msg), "Enter a valid PS5 IP address.");
                            focus = 0; continue;
                        }
                        if (!ui_write_host(config_path, ip)) {
                            snprintf(msg, sizeof(msg), "Failed to save host to config.json");
                            continue;
                        }
                        ui_reload_cfg(cfg, config_path);
                        if (!ui_has_keys(cfg)) {
                            snprintf(msg, sizeof(msg), "Missing registration keys. Import chiaki-ng-Default.ini first.");
                            focus = 13; continue;
                        }
                        return UI_RESULT_CONNECT;
                    }
                }

            } else if (ev.type == SDL_TEXTINPUT) {
                if (focus == 0 || editing) {
                    for (const char *p = ev.text.text; *p; p++) {
                        char c = *p;
                        if (!(isdigit((unsigned char)c) || c == '.')) continue;
                        size_t len = strlen(ip);
                        if (len + 1 < sizeof(ip)) { ip[len] = c; ip[len + 1] = '\0'; }
                    }
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                    if (editing) { editing = false; focus = 0; continue; }
                    return UI_RESULT_QUIT;
                }
                SDL_Keycode fake_sym = 0;
                if      (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    fake_sym = SDLK_UP;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  fake_sym = SDLK_DOWN;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  fake_sym = SDLK_LEFT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) fake_sym = SDLK_RIGHT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)          fake_sym = SDLK_RETURN;
                if (fake_sym) {
                    SDL_Event fake; memset(&fake, 0, sizeof(fake));
                    fake.type = SDL_KEYDOWN; fake.key.keysym.sym = fake_sym;
                    SDL_PushEvent(&fake);
                }
            }
        }

        render_setup_screen(renderer, status, ip, focus, msg,
                             has_keys, can_import, can_connect, editing, input);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
}
