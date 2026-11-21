#pragma once
/* Shared look and helpers for the menu screens (ui.c, ui_home.c). */

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "config.h"
#include "ui_text.h"

struct InputContext;

/* ── layout (1920x1080 logical, scaled to the TV's UI plane) ──────────────── */
#define SCREEN_W    1920
#define SCREEN_H    1080
#define HDR_Y       24
#define HDR_H       72
#define CONTENT_Y   (HDR_Y + HDR_H + 20)

/* webOS remote keys.  RED is an in-app fallback for Back. */
#define WEBOS_KEY_BACK 1073742094
#define WEBOS_KEY_RED  403

/* ── colour palette (R,G,B,A) ──────────────────────────────────────────────── */
#define COL_BG              0x04, 0x07, 0x12, 0xff   /* deep navy */
#define COL_BG_DEEPER       0x02, 0x04, 0x0c, 0xff
#define COL_PANEL           0x09, 0x11, 0x28, 0xff   /* panels / footer */
#define COL_ELEVATED        0x0d, 0x17, 0x32, 0xff
#define COL_CARD            0x14, 0x20, 0x44, 0xff   /* button / input BG */
#define COL_CODE_BG         0x07, 0x0e, 0x22, 0xff
#define COL_ACCENT          0x00, 0x72, 0xff, 0xff   /* PlayStation blue */
#define COL_ACCENT_MID      0x00, 0x48, 0xc0, 0xff
#define COL_ACCENT_DARK     0x00, 0x2e, 0x82, 0xff
#define COL_SUCCESS         0x00, 0xb8, 0x58, 0xff
#define COL_SUCCESS_BG      0x00, 0x28, 0x14, 0xff
#define COL_WARNING         0xf8, 0x9c, 0x1c, 0xff
#define COL_WARNING_BG      0x30, 0x1e, 0x04, 0xff
#define COL_ERROR           0xff, 0x44, 0x50, 0xff
#define COL_ERROR_BG        0x30, 0x08, 0x0c, 0xff
#define COL_BORDER          0x1c, 0x2c, 0x58, 0xff
#define COL_BORDER_DIM      0x10, 0x18, 0x36, 0xff
#define COL_TEXT            0xec, 0xf2, 0xff, 0xff
#define COL_TEXT_DIM        0x78, 0x8c, 0xb8, 0xff
#define COL_TEXT_FAINT      0x3c, 0x4c, 0x78, 0xff
#define COL_TEXT_CODE       0x48, 0xcc, 0xff, 0xff

static inline SDL_Color ui_rgba(int R, int G, int B, int A)
{
    SDL_Color c = { (Uint8)R, (Uint8)G, (Uint8)B, (Uint8)A };
    return c;
}
#define C(name) ui_rgba(name)

/* ── per-console stream options ────────────────────────────────────────────── */
typedef struct { int w, h; const char *label; } ResOption;
/* No 1440p/2160p: PS5 Remote Play streams at up to 1080p and refuses larger
 * sizes ("AvCap failed to initialize video"). */
static const ResOption RES_OPTIONS[] = { {1280, 720, "720p"}, {1920, 1080, "1080p"} };
#define N_RES ((int)(sizeof(RES_OPTIONS) / sizeof(RES_OPTIONS[0])))
static const int FPS_OPTIONS[] = { 30, 60 };
#define N_FPS ((int)(sizeof(FPS_OPTIONS) / sizeof(FPS_OPTIONS[0])))
static const int BITRATE_OPTIONS[] = { 5000, 10000, 15000, 25000, 35000, 45000 };
#define N_BITRATE ((int)(sizeof(BITRATE_OPTIONS) / sizeof(BITRATE_OPTIONS[0])))
static const char *const CODEC_OPTIONS[] = { "h264", "h265", "h265_hdr" };
static const char *const CODEC_LABELS[]  = { "H.264 (AVC)", "H.265 (HEVC)", "H.265 HDR" };
#define N_CODEC 3
static const char *const SLEEP_LABELS[] = { "Off", "On", "Ask" };   /* SLEEP_OFF/ON/ASK */

/* ── drawing helpers (ui.c) ────────────────────────────────────────────────── */
void set_color(SDL_Renderer *r, int R, int G, int B, int A);
void fill_rect(SDL_Renderer *r, int x, int y, int w, int h);
void hline(SDL_Renderer *r, int x, int y, int w);
void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius);
/* The app logo ("Couch Pad" mark) as a size x size tile. */
void ui_draw_logo(SDL_Renderer *r, int x, int y, int size);
void stroke_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius);
void draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale);
int  text_width(const char *text, int scale);
void draw_button(SDL_Renderer *r, int x, int y, int w, int h, const char *label, bool focused, bool enabled);
void big_button(SDL_Renderer *r, int x, int y, int w, int h, const char *label,
                bool focused, bool enabled, bool primary);
void ui_draw_option_row(SDL_Renderer *r, int x, int y, int w, int h,
                        const char *label, const char *value, bool focused);
/* Header band with a title, used by the settings-style screens. */
void ui_draw_header(SDL_Renderer *r, const char *title, const char *right_note);
/* Help panel + footer used by the settings-style screens. */
void ui_draw_help_footer(SDL_Renderer *r, const char *help, const char *keys, const char *msg);
/* Gamepad → arrow/OK/Back key events while a menu is open. */
void pad_nav_poll(struct InputContext *in);
const char *on_off(bool v);
bool ui_is_back_key(SDL_Keycode k);
/* PSN sign-in screen (QR code + code entry); adds the account to cfg and
 * returns its index, -1 if cancelled (ui_home.c). */
int  ui_psn_sign_in(SDL_Renderer *r, struct InputContext *in, AppConfig *cfg, const char *config_path);
/* Signed-in PSN accounts: list, sign out, sign in (ui_home.c). */
void ui_psn_accounts(SDL_Renderer *r, struct InputContext *in, AppConfig *cfg, const char *config_path);
