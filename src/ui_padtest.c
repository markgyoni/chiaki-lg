#include "ui_padtest.h"
#include "ds_audio.h"
#include "ui_text.h"
#include "input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 1920
#define H 1080

/* TV remote codes seen by SDL on webOS (see ui.c / input.c) */
#define KEY_BACK 1073742094
#define KEY_RED  403

#define PS_HOLD_EXIT_MS 2000
#define RUMBLE_TEST_MS  700

static const SDL_Color C_BG      = { 0x04, 0x07, 0x12, 0xff };
static const SDL_Color C_PANEL   = { 0x09, 0x11, 0x28, 0xff };
static const SDL_Color C_BODY    = { 0x14, 0x20, 0x44, 0xff };
static const SDL_Color C_IDLE    = { 0x2a, 0x3a, 0x6a, 0xff };
static const SDL_Color C_ACCENT  = { 0x00, 0x72, 0xff, 0xff };
static const SDL_Color C_HOT     = { 0x40, 0xa8, 0xff, 0xff };
static const SDL_Color C_TEXT    = { 0xec, 0xf2, 0xff, 0xff };
static const SDL_Color C_DIM     = { 0x9a, 0xaa, 0xd0, 0xff };
static const SDL_Color C_OK      = { 0x00, 0xc8, 0x60, 0xff };
static const SDL_Color C_WARN    = { 0xf8, 0x9c, 0x1c, 0xff };

/* ── primitives ──────────────────────────────────────────────────────────── */

static void col(SDL_Renderer *r, SDL_Color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }

static void rect(SDL_Renderer *r, int x, int y, int w, int h)
{
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

static void circle(SDL_Renderer *r, int cx, int cy, int rad)
{
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)sqrtf((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void rrect(SDL_Renderer *r, int x, int y, int w, int h, int rad)
{
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    rect(r, x + rad, y, w - 2 * rad, h);
    rect(r, x, y + rad, rad, h - 2 * rad);
    rect(r, x + w - rad, y + rad, rad, h - 2 * rad);
    circle(r, x + rad, y + rad, rad);
    circle(r, x + w - rad - 1, y + rad, rad);
    circle(r, x + rad, y + h - rad - 1, rad);
    circle(r, x + w - rad - 1, y + h - rad - 1, rad);
}

static void thick_line(SDL_Renderer *r, int x1, int y1, int x2, int y2, int t)
{
    for (int i = -t / 2; i <= t / 2; i++) {
        SDL_RenderDrawLine(r, x1 + i, y1, x2 + i, y2);
        SDL_RenderDrawLine(r, x1, y1 + i, x2, y2 + i);
    }
}

static void text(SDL_Renderer *r, int x, int y, const char *s, int px, UiFontWeight w, SDL_Color c, UiAlign a)
{
    ui_text_draw(r, x, y, s, px, w, c, a);
}

/* ── controller drawing ──────────────────────────────────────────────────── */

static bool on(const ChiakiControllerState *st, uint32_t b) { return (st->buttons & b) != 0; }

static void trigger_bar(SDL_Renderer *r, int x, int y, const char *name, uint8_t v)
{
    col(r, C_IDLE); rrect(r, x, y, 260, 44, 10);
    col(r, v ? C_ACCENT : C_IDLE); if (v) rrect(r, x, y, 26 + (234 * v) / 255, 44, 10);
    char s[32]; snprintf(s, sizeof(s), "%s  %u", name, v);
    text(r, x + 130, y + 2, s, 30, UI_FONT_SEMIBOLD, C_TEXT, UI_ALIGN_CENTER);
}

static void pill(SDL_Renderer *r, int x, int y, int w, int h, const char *name, bool pressed)
{
    col(r, pressed ? C_ACCENT : C_IDLE); rrect(r, x, y, w, h, h / 2);
    text(r, x + w / 2, y + (h - ui_text_line_height(28)) / 2, name, 28, UI_FONT_SEMIBOLD, C_TEXT, UI_ALIGN_CENTER);
}

static void face_button(SDL_Renderer *r, int cx, int cy, int shape, bool pressed)
{
    const int R = 40;
    col(r, pressed ? C_ACCENT : C_IDLE); circle(r, cx, cy, R);
    col(r, C_TEXT);
    switch (shape) {
    case 0: /* triangle */
        thick_line(r, cx, cy - 20, cx - 20, cy + 14, 4);
        thick_line(r, cx, cy - 20, cx + 20, cy + 14, 4);
        thick_line(r, cx - 20, cy + 14, cx + 20, cy + 14, 4);
        break;
    case 1: /* circle */
        circle(r, cx, cy, 20); col(r, pressed ? C_ACCENT : C_IDLE); circle(r, cx, cy, 15);
        break;
    case 2: /* cross */
        thick_line(r, cx - 17, cy - 17, cx + 17, cy + 17, 4);
        thick_line(r, cx - 17, cy + 17, cx + 17, cy - 17, 4);
        break;
    default: /* square */
        rect(r, cx - 17, cy - 17, 34, 4); rect(r, cx - 17, cy + 13, 34, 4);
        rect(r, cx - 17, cy - 17, 4, 34); rect(r, cx + 13, cy - 17, 4, 34);
        break;
    }
}

static void stick(SDL_Renderer *r, int cx, int cy, int16_t x, int16_t y, bool click, const char *name)
{
    const int R = 96;
    col(r, click ? C_ACCENT : C_IDLE); circle(r, cx, cy, R);
    col(r, C_BODY); circle(r, cx, cy, R - 6);
    col(r, C_IDLE); rect(r, cx - R + 6, cy, 2 * R - 12, 1); rect(r, cx, cy - R + 6, 1, 2 * R - 12);
    int dx = (int)((long)x * (R - 22) / 32767), dy = (int)((long)y * (R - 22) / 32767);
    col(r, click ? C_HOT : C_ACCENT); circle(r, cx + dx, cy + dy, 22);
    char s[48]; snprintf(s, sizeof(s), "%s  %+.2f %+.2f", name, x / 32767.0, y / 32767.0);
    text(r, cx, cy + R + 10, s, 28, UI_FONT_REGULAR, C_DIM, UI_ALIGN_CENTER);
}

static void draw_controller(SDL_Renderer *r, const ChiakiControllerState *st)
{
    /* body */
    col(r, C_BODY);
    rrect(r, 110, 250, 1040, 420, 80);
    rrect(r, 110, 450, 300, 360, 120);
    rrect(r, 850, 450, 300, 360, 120);

    trigger_bar(r, 150, 130, "L2", st->l2_state);
    trigger_bar(r, 850, 130, "R2", st->r2_state);
    pill(r, 150, 190, 260, 46, "L1", on(st, CHIAKI_CONTROLLER_BUTTON_L1));
    pill(r, 850, 190, 260, 46, "R1", on(st, CHIAKI_CONTROLLER_BUTTON_R1));

    /* touchpad with live touch points (console coordinates 1920x1080) */
    const int TX = 440, TY = 270, TW = 380, TH = 200;
    bool tclick = on(st, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD);
    col(r, tclick ? C_ACCENT : C_IDLE); rrect(r, TX - 4, TY - 4, TW + 8, TH + 8, 16);
    col(r, C_PANEL); rrect(r, TX, TY, TW, TH, 14);
    text(r, TX + TW / 2, TY + TH / 2 - 18, tclick ? "Touchpad click" : "Touchpad", 28, UI_FONT_REGULAR,
         C_DIM, UI_ALIGN_CENTER);
    for (int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++) {
        if (st->touches[i].id < 0) continue;
        int px = TX + (int)st->touches[i].x * TW / 1920, py = TY + (int)st->touches[i].y * TH / 1080;
        col(r, i ? C_WARN : C_HOT); circle(r, px, py, 16);
        char n[4]; snprintf(n, sizeof(n), "%d", i + 1);
        text(r, px, py - ui_text_line_height(24) / 2, n, 24, UI_FONT_BOLD, C_BG, UI_ALIGN_CENTER);
    }

    pill(r, 300, 280, 120, 44, "Create", on(st, CHIAKI_CONTROLLER_BUTTON_SHARE));
    pill(r, 840, 280, 130, 44, "Options", on(st, CHIAKI_CONTROLLER_BUTTON_OPTIONS));

    /* d-pad */
    const int DX = 270, DY = 420, S = 58;
    col(r, on(st, CHIAKI_CONTROLLER_BUTTON_DPAD_UP) ? C_ACCENT : C_IDLE);    rrect(r, DX - S / 2, DY - S - S / 2, S, S, 10);
    col(r, on(st, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN) ? C_ACCENT : C_IDLE);  rrect(r, DX - S / 2, DY + S / 2, S, S, 10);
    col(r, on(st, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT) ? C_ACCENT : C_IDLE);  rrect(r, DX - S - S / 2, DY - S / 2, S, S, 10);
    col(r, on(st, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT) ? C_ACCENT : C_IDLE); rrect(r, DX + S / 2, DY - S / 2, S, S, 10);

    /* face buttons */
    const int FX = 990, FY = 420, G = 66;
    face_button(r, FX, FY - G, 0, on(st, CHIAKI_CONTROLLER_BUTTON_PYRAMID));
    face_button(r, FX + G, FY, 1, on(st, CHIAKI_CONTROLLER_BUTTON_MOON));
    face_button(r, FX, FY + G, 2, on(st, CHIAKI_CONTROLLER_BUTTON_CROSS));
    face_button(r, FX - G, FY, 3, on(st, CHIAKI_CONTROLLER_BUTTON_BOX));

    /* PS button */
    col(r, on(st, CHIAKI_CONTROLLER_BUTTON_PS) ? C_ACCENT : C_IDLE); circle(r, 630, 560, 34);
    text(r, 630, 560 - ui_text_line_height(28) / 2, "PS", 28, UI_FONT_BOLD, C_TEXT, UI_ALIGN_CENTER);

    stick(r, 450, 640, st->left_x, st->left_y, on(st, CHIAKI_CONTROLLER_BUTTON_L3), "L3");
    stick(r, 810, 640, st->right_x, st->right_y, on(st, CHIAKI_CONTROLLER_BUTTON_R3), "R3");
}

/* ── right panel: controllers + motion ───────────────────────────────────── */

static void draw_info(SDL_Renderer *r, struct InputContext *in, const ChiakiControllerState *st)
{
    const int X = 1230, Y = 120, PW = 640;
    col(r, C_PANEL); rrect(r, X, Y, PW, 720, 18);
    int y = Y + 24;
    text(r, X + 30, y, "Connected controllers", 34, UI_FONT_BOLD, C_TEXT, UI_ALIGN_LEFT);
    y += 56;

    InputPadInfo pads[4];
    int n = input_get_pads(in, pads, 4);
    if (n == 0) {
        y += ui_text_draw_wrapped(r, X + 30, y, PW - 60,
                                  "No controller found. Turn one on or plug it in by USB.",
                                  28, UI_FONT_REGULAR, C_WARN, 3);
    }
    int shown = 0;
    for (int i = 0; i < n && i < 2 && y < Y + 260; i++, shown++) {
        ui_text_draw_fit(r, X + 30, y, PW - 60, pads[i].name, 30, UI_FONT_SEMIBOLD, C_TEXT); y += 40;
        char s[160];
        snprintf(s, sizeof(s), "%s  |  layout %s%s", pads[i].connection, pads[i].profile,
                 pads[i].raw_hid ? "  |  touchpad + motion" : "");
        y += ui_text_draw_wrapped(r, X + 30, y, PW - 60, s, 26, UI_FONT_REGULAR, C_DIM, 2) + 4;
        y += ui_text_draw_wrapped(r, X + 30, y, PW - 60, pads[i].outputs, 26, UI_FONT_REGULAR,
                                  pads[i].outputs_active ? C_OK : C_WARN, 2);
        y += 18;
    }
    if (n > shown) {
        char s[48]; snprintf(s, sizeof(s), "+ %d more", n - shown);
        text(r, X + 30, y, s, 26, UI_FONT_REGULAR, C_DIM, UI_ALIGN_LEFT); y += 40;
    }

    /* motion */
    y = Y + 430;
    text(r, X + 30, y, "Motion", 34, UI_FONT_BOLD, C_TEXT, UI_ALIGN_LEFT);
    char s[96];
    snprintf(s, sizeof(s), "Gyro   %+6.2f %+6.2f %+6.2f", st->gyro_x, st->gyro_y, st->gyro_z);
    text(r, X + 30, y + 56, s, 28, UI_FONT_REGULAR, C_DIM, UI_ALIGN_LEFT);
    snprintf(s, sizeof(s), "Accel  %+6.2f %+6.2f %+6.2f", st->accel_x, st->accel_y, st->accel_z);
    text(r, X + 30, y + 96, s, 28, UI_FONT_REGULAR, C_DIM, UI_ALIGN_LEFT);
    ui_text_draw_wrapped(r, X + 30, y + 150, PW - 300, "Tilt the controller to move the dot.", 26, UI_FONT_REGULAR, C_DIM, 2);

    /* spirit level from the accelerometer (x = roll, z = pitch, in g) */
    const int BX = X + PW - 150, BY = y + 150, BR = 100;
    col(r, C_IDLE); circle(r, BX, BY, BR);
    col(r, C_BODY); circle(r, BX, BY, BR - 5);
    col(r, C_IDLE); circle(r, BX, BY, 24); col(r, C_BODY); circle(r, BX, BY, 20);
    float ax = st->accel_x, az = st->accel_z;
    if (ax > 1) ax = 1; if (ax < -1) ax = -1; if (az > 1) az = 1; if (az < -1) az = -1;
    bool motion = st->accel_x != 0 || st->accel_y != 0 || st->accel_z != 0;
    col(r, motion ? C_HOT : C_IDLE);
    circle(r, BX + (int)(ax * (BR - 24)), BY + (int)(az * (BR - 24)), 18);
    if (!motion)
        text(r, X + 30, y + 196, "No motion data", 26, UI_FONT_REGULAR, C_WARN, UI_ALIGN_LEFT);
}

/* ── output tests ────────────────────────────────────────────────────────── */

enum { T_RUMBLE, T_HAPTICS, T_SPEAKER, T_TRIGGERS, T_LIGHTBAR, T_PLAYER, T_DONE, T_COUNT };

static const SDL_Color LIGHTBAR_COLOURS[] = {
    { 0xff, 0x00, 0x00, 0xff }, { 0x00, 0xff, 0x00, 0xff }, { 0x00, 0x40, 0xff, 0xff },
    { 0xff, 0x00, 0xc0, 0xff }, { 0xff, 0xff, 0xff, 0xff },
};
static const char *LIGHTBAR_NAMES[] = { "red", "green", "blue", "pink", "white" };

static void set_triggers(struct InputContext *in, bool resist)
{
    /* Mode 0x01: continuous resistance from position p[0] with force p[1]. */
    uint8_t p[10] = { 0 };
    if (resist) { p[0] = 0x30; p[1] = 0xff; }
    input_set_trigger_effects(in, resist ? 0x01 : 0x05, p, resist ? 0x01 : 0x05, p);
}

static void draw_tests(SDL_Renderer *r, int focus, bool triggers_on, int light_idx, int player)
{
    const int Y = 880, BH = 86, GAP = 22, X0 = 60;
    const int BW = (W - 2 * X0 - (T_COUNT - 1) * GAP) / T_COUNT;
    char lbl[T_COUNT][48];
    snprintf(lbl[T_RUMBLE], 48, "Vibrate");
    snprintf(lbl[T_HAPTICS], 48, "HD haptics");
    snprintf(lbl[T_SPEAKER], 48, "Speaker");
    snprintf(lbl[T_TRIGGERS], 48, "Triggers: %s", triggers_on ? "on" : "off");
    snprintf(lbl[T_LIGHTBAR], 48, "Light: %s", light_idx < 0 ? "-" : LIGHTBAR_NAMES[light_idx]);
    snprintf(lbl[T_PLAYER], 48, "LEDs: %d", player);
    snprintf(lbl[T_DONE], 48, "Done");
    for (int i = 0; i < T_COUNT; i++) {
        int x = X0 + i * (BW + GAP);
        if (i == focus) { col(r, C_TEXT); rrect(r, x - 4, Y - 4, BW + 8, BH + 8, 18); }
        col(r, i == focus ? C_ACCENT : C_BODY); rrect(r, x, Y, BW, BH, 14);
        text(r, x + BW / 2, Y + (BH - ui_text_line_height(30)) / 2, lbl[i], 30, UI_FONT_SEMIBOLD,
             C_TEXT, UI_ALIGN_CENTER);
    }
}

/* USB audio status above the test buttons (HD haptics, speaker, microphone). */
static void draw_audio_status(SDL_Renderer *r, DsAudio *a)
{
    char s[200];
    float lvl = 0;
    int mic = ds_audio_mic_state(a, &lvl);
    SDL_Color c = C_DIM;
    if (!ds_audio_card_present(a)) {
        snprintf(s, sizeof(s), "USB audio: connect the DualSense by USB for HD haptics, speaker and microphone");
    } else {
        const char *out = ds_audio_haptics_active(a) ? "output open" : "output NOT open";
        if (mic == 2) {
            int bars = (int)(lvl * 400.0f);
            if (bars > 20) bars = 20;
            char meter[24];
            for (int i = 0; i < 20; i++) meter[i] = i < bars ? '|' : '.';
            meter[20] = '\0';
            snprintf(s, sizeof(s), "USB audio: %s   |   Microphone: receiving  %s", out, meter);
        } else {
            snprintf(s, sizeof(s), "USB audio: %s   |   Microphone: %s", out,
                     mic == -2 ? "no data (TV USB capture error)" : "starting...");
            if (mic == -2) c = C_WARN;
        }
    }
    ui_text_draw_fit(r, 60, 830, 1110, s, 26, UI_FONT_REGULAR, c);   /* left of the info panel */
}

/* ── screen loop ─────────────────────────────────────────────────────────── */
void ui_run_padtest(SDL_Renderer *r, struct InputContext *in)
{
    if (!r) return;
    ui_text_init(r);
    int focus = T_RUMBLE, light_idx = -1, player = 0;
    bool triggers_on = false;
    Uint32 rumble_until = 0, ps_since = 0;
    /* DualSense USB audio for the HD haptics / speaker / microphone checks */
    DsAudio *audio = ds_audio_new(true, true, NULL, NULL);
    ds_audio_mic_enable(audio, true);

    for (;;) {
        SDL_Event ev;
        bool exit = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { exit = true; break; }
            if (ev.type != SDL_KEYDOWN) continue;
            SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_ESCAPE || k == KEY_BACK || k == KEY_RED) { exit = true; break; }
            if (k == SDLK_LEFT  && focus > 0)           focus--;
            if (k == SDLK_RIGHT && focus < T_COUNT - 1) focus++;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                switch (focus) {
                case T_RUMBLE:   input_set_rumble(in, 220, 220); rumble_until = SDL_GetTicks() + RUMBLE_TEST_MS; break;
                case T_HAPTICS:  ds_audio_test_tone(audio, 0xc, 1500); break;
                case T_SPEAKER:  ds_audio_test_tone(audio, 0x3, 1000); break;
                case T_TRIGGERS: triggers_on = !triggers_on; set_triggers(in, triggers_on); break;
                case T_LIGHTBAR: {
                    light_idx = (light_idx + 1) % (int)(sizeof(LIGHTBAR_COLOURS) / sizeof(LIGHTBAR_COLOURS[0]));
                    SDL_Color c = LIGHTBAR_COLOURS[light_idx];
                    input_set_lightbar(in, c.r, c.g, c.b);
                    break;
                }
                case T_PLAYER:   player = player % 5 + 1; input_set_player_index(in, (uint8_t)player); break;
                case T_DONE:     exit = true; break;
                }
            }
        }

        ChiakiControllerState st;
        input_get_state(in, &st);
        Uint32 now = SDL_GetTicks();
        if (rumble_until && (Sint32)(now - rumble_until) >= 0) { input_set_rumble(in, 0, 0); rumble_until = 0; }
        if (on(&st, CHIAKI_CONTROLLER_BUTTON_PS)) {
            if (!ps_since) ps_since = now ? now : 1;
            else if (now - ps_since >= PS_HOLD_EXIT_MS) exit = true;
        } else {
            ps_since = 0;
        }
        if (exit) break;

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        col(r, C_BG); rect(r, 0, 0, W, H);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        text(r, 60, 34, "Controller test", 52, UI_FONT_BOLD, C_TEXT, UI_ALIGN_LEFT);
        text(r, W - 60, 46, "What you see here is exactly what the PS5 receives", 28, UI_FONT_REGULAR,
             C_DIM, UI_ALIGN_RIGHT);
        draw_controller(r, &st);
        draw_info(r, in, &st);
        draw_tests(r, focus, triggers_on, light_idx, player);
        draw_audio_status(r, audio);
        text(r, W / 2, 1000, ps_since ? "Keep holding PS to exit..."
                                      : "Remote: Left/Right choose a test, OK runs it, Back exits.   "
                                        "Controller: hold PS for 2 s to exit.",
             26, UI_FONT_REGULAR, ps_since ? C_WARN : C_DIM, UI_ALIGN_CENTER);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    ds_audio_free(audio);

    /* leave the pads as the launcher found them */
    input_set_rumble(in, 0, 0);
    set_triggers(in, false);
    input_set_lightbar(in, 0x00, 0x00, 0x40);
    input_set_player_index(in, 0);

    /* swallow the key / button that ended the screen */
    SDL_Delay(150);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}
