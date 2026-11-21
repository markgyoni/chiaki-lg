/* Start screen with the console list, plus everything for managing consoles:
 * add (find on the network → PSN sign-in → PIN → register), per-console
 * settings, pair again, remove.  Replaces the chiaki-ng .ini import. */

#include "ui.h"
#include "ui_common.h"
#include "ui_padtest.h"
#include "console_net.h"
#include "psn_login.h"
#include "input.h"
#include "app_log.h"
#include "third_party/qrcodegen.h"


#include <arpa/inet.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "?"
#endif
#ifndef APP_GIT
#define APP_GIT "?"
#endif

/* ── small helpers ─────────────────────────────────────────────────────────── */

static bool valid_ipv4(const char *s)
{
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}

static bool console_registered(const ConsoleConfig *c)
{
    return c->registered_key_b64 && c->registered_key_b64[0] && c->rp_key_b64 && c->rp_key_b64[0] &&
           c->psn_account_id_b64 && c->psn_account_id_b64[0];
}

static void drain_events(void)
{
    SDL_Delay(120);
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}
    ui_pointer_reset();
}

/* Next key from the remote, a gamepad or the Magic Remote pointer
 * (arrows/OK/Back; Back is always SDLK_ESCAPE), 0 if none.  Pointer moves
 * are picked up by the caller with ui_pointer_hover(). */
static SDL_Keycode poll_key(struct InputContext *in, bool *quit)
{
    pad_nav_poll(in);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { if (quit) *quit = true; return SDLK_ESCAPE; }
        SDL_Keycode k = ui_menu_key(&ev);
        if (k) return k;
    }
    return 0;
}

static bool is_ok(SDL_Keycode k) { return k == SDLK_RETURN || k == SDLK_KP_ENTER; }

static void panel(SDL_Renderer *r, int x, int y, int w, int h)
{
    set_color(r, COL_PANEL);
    fill_rounded_rect(r, x, y, w, h, 18);
}

/* Run fn(arg) on a thread while a loading screen with `text` animates. */
typedef struct { void (*fn)(void *); void *arg; volatile bool done; } BusyJob;
static void *busy_thread(void *p) { BusyJob *j = p; j->fn(j->arg); j->done = true; return NULL; }
static void run_busy(SDL_Renderer *r, const char *text, void (*fn)(void *), void *arg)
{
    BusyJob j = { fn, arg, false };
    pthread_t t;
    if (pthread_create(&t, NULL, busy_thread, &j) != 0) { fn(arg); return; }
    while (!j.done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        ui_render_loading(r, text);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    pthread_join(t, NULL);
    drain_events();
}

/* Yes/No dialog over a dimmed screen (the caller draws the background first). */
static bool confirm(SDL_Renderer *r, struct InputContext *in, const char *question,
                    const char *detail, const char *yes, const char *no)
{
    int focus = 1;
    drain_events();
    for (;;) {
        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv == 0 || hv == 1) focus = hv;
        if (quit || ui_is_back_key(k)) return false;
        if (k == SDLK_LEFT) focus = 0;
        if (k == SDLK_RIGHT) focus = 1;
        if (is_ok(k)) { drain_events(); return focus == 0; }

        ui_hit_begin();
        set_color(r, COL_BG);
        fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
        const int BW = 1000, BH = 400, BX = (SCREEN_W - BW) / 2, BY = (SCREEN_H - BH) / 2;
        panel(r, BX, BY, BW, BH);
        set_color(r, COL_BORDER);
        stroke_rounded_rect(r, BX, BY, BW, BH, 18);
        ui_text_draw(r, SCREEN_W / 2, BY + 50, question, 44, UI_FONT_BOLD, C(COL_TEXT), UI_ALIGN_CENTER);
        ui_text_draw_wrapped(r, BX + 70, BY + 124, BW - 140, detail, 28, UI_FONT_REGULAR, C(COL_TEXT_DIM), 3);
        const int W = 360, H = 100, Y = BY + BH - H - 44, X0 = SCREEN_W / 2 - W - 20;
        big_button(r, X0, Y, W, H, yes, focus == 0, true, false);
        big_button(r, X0 + W + 40, Y, W, H, no, focus == 1, true, false);
        ui_hit_add(0, X0, Y, W, H);
        ui_hit_add(1, X0 + W + 40, Y, W, H);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
}

/* ── text / number entry ───────────────────────────────────────────────────── */

/* Text entry with the TV's own on-screen keyboard: SDL_StartTextInput()
 * makes webOS show its virtual keyboard, typed text arrives as SDL_TEXTINPUT.
 * digits_only keeps 0-9 (and '.' if allow_dot) for addresses and PINs.
 * With the keyboard closed: OK on the field opens it again, Done / Cancel
 * below.  Returns false on Back / Cancel. */
static bool char_allowed(char ch, bool digits_only, bool allow_dot)
{
    if (digits_only) return (ch >= '0' && ch <= '9') || (allow_dot && ch == '.');
    return ch >= 0x20 && ch < 0x7f;
}

static void append_text(char *work, size_t max_len, const char *text, bool digits_only, bool allow_dot)
{
    for (const char *p = text; *p; p++) {
        size_t l = strlen(work);
        if (l >= max_len) break;
        if (!char_allowed(*p, digits_only, allow_dot)) continue;
        work[l] = *p;
        work[l + 1] = '\0';
    }
}

static void open_tv_keyboard(const SDL_Rect *field)
{
    SDL_StopTextInput();                         /* restart, so the keyboard shows again */
    SDL_SetTextInputRect((SDL_Rect *)field);
    SDL_StartTextInput();
}

/* main.c grabs the app window; while typing, let go of it so the Magic
 * Remote pointer can reach the TV keyboard (a separate surface). */
static void typing_pointer(SDL_Window *win, bool typing)
{
    if (win) SDL_SetWindowGrab(win, typing ? SDL_FALSE : SDL_TRUE);
}

static bool text_input(SDL_Renderer *r, struct InputContext *in, const char *title, const char *help,
                       char *buf, size_t cap, bool digits_only, bool allow_dot, size_t max_len)
{
    char work[128], pre[64] = "";
    snprintf(work, sizeof(work), "%s", buf);
    if (max_len == 0 || max_len >= sizeof(work)) max_len = sizeof(work) - 1;
    /* (LG's SDL has no SDL_RenderGetWindow; main.c grabs the app window) */
    SDL_Window *win = SDL_GetGrabbedWindow();
    if (!win) win = SDL_GetKeyboardFocus();
    if (!win) win = SDL_GetMouseFocus();
    const int TOP = CONTENT_Y + 20, FX = 120, FW = SCREEN_W - 240, FH = 110, FY = TOP + 120;
    SDL_Rect field = { FX, FY, FW, FH };
    int focus = 0;                               /* 0 field, 1 Done, 2 Cancel */
    Uint32 kb_seen = 0;                          /* last time the keyboard was up */
    bool result = false, finished = false, kb_was_up = false;
    const int BY = FY + FH + 40, BW = 340, BH = 96, CX = FX + BW + 24;
    drain_events();
    typing_pointer(win, true);
    open_tv_keyboard(&field);
    kb_seen = SDL_GetTicks();

    while (!finished) {
        bool kb_up = win && SDL_IsScreenKeyboardShown(win);
        if (kb_up != kb_was_up) {
            app_log_always("[UI] TV keyboard %s\n", kb_up ? "shown" : "hidden");
            kb_was_up = kb_up;
            if (!kb_up) SDL_StopTextInput();     /* else the IME keeps eating Back */
        }
        if (kb_up) kb_seen = SDL_GetTicks();
        bool kb_recent = SDL_GetTicks() - kb_seen < 400;

        pad_nav_poll(in);
        SDL_Event ev;
        while (!finished && SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { finished = true; break; }
            if (ev.type == SDL_TEXTINPUT) {
                pre[0] = '\0';
                append_text(work, max_len, ev.text.text, digits_only, allow_dot);
                continue;
            }
            if (ev.type == SDL_TEXTEDITING) {
                snprintf(pre, sizeof(pre), "%s", ev.edit.text);
                continue;
            }
            SDL_Keycode k = ui_menu_key(&ev);
            int hv = ui_pointer_hover();
            if (hv >= 0 && hv <= 2) focus = hv;
            if (!k) continue;
            if (k == SDLK_BACKSPACE) {
                size_t l = strlen(work);
                if (l) work[l - 1] = '\0';
            } else if (ui_is_back_key(k)) {
                /* Back closes the TV keyboard first; only a second Back cancels */
                if (!kb_up && !kb_recent) finished = true;
            } else if (is_ok(k)) {
                /* a click acts on what it hit; Enter on the TV keyboard means Done */
                int act = ui_pointer_clicked() ? focus : kb_up ? 1 : focus;
                if (act == 1) {
                    append_text(work, max_len, pre, digits_only, allow_dot);
                    snprintf(buf, cap, "%s", work);
                    result = finished = true;
                } else if (act == 2) {
                    finished = true;
                } else if (!kb_up) {
                    open_tv_keyboard(&field);
                    kb_seen = SDL_GetTicks();
                }
            } else if (!kb_up) {
                if (k == SDLK_DOWN && focus == 0) focus = 1;
                else if (k == SDLK_UP) focus = 0;
                else if (k == SDLK_LEFT && focus == 2) focus = 1;
                else if (k == SDLK_RIGHT && focus == 1) focus = 2;
                else if ((k >= SDLK_0 && k <= SDLK_9) || (k >= SDLK_KP_0 && k <= SDLK_KP_9)) {
                    /* the remote's number keys with the keyboard closed */
                    char t[2] = { (char)('0' + (k >= SDLK_KP_0 ? (k == SDLK_KP_0 ? 0 : k - SDLK_KP_1 + 1)
                                                               : k - SDLK_0)), 0 };
                    append_text(work, max_len, t, digits_only, allow_dot);
                }
            }
        }
        if (finished) break;

        ui_draw_header(r, title, NULL);
        ui_hit_add(0, FX, FY, FW, FH);
        ui_hit_add(1, FX, BY, BW, BH);
        ui_hit_add(2, CX, BY, BW, BH);
        ui_text_draw_wrapped(r, FX, TOP, FW, help, 30, UI_FONT_REGULAR, C(COL_TEXT_DIM), 3);
        bool ff = focus == 0 && !kb_up;
        if (ff) { set_color(r, COL_TEXT); fill_rounded_rect(r, FX - 4, FY - 4, FW + 8, FH + 8, 18); }
        set_color(r, COL_CODE_BG);
        fill_rounded_rect(r, FX, FY, FW, FH, 14);
        set_color(r, COL_ACCENT);
        stroke_rounded_rect(r, FX, FY, FW, FH, 14);
        char shown[200];
        snprintf(shown, sizeof(shown), "%s%s%s", work, pre, (SDL_GetTicks() / 500) % 2 ? "_" : " ");
        ui_text_draw(r, FX + 30, FY + (FH - ui_text_line_height(56)) / 2, shown, 56,
                     UI_FONT_SEMIBOLD, C(COL_TEXT), UI_ALIGN_LEFT);
        big_button(r, FX, BY, BW, BH, "Done", focus == 1 && !kb_up, true, true);
        big_button(r, CX, BY, BW, BH, "Cancel", focus == 2 && !kb_up, true, false);
        ui_text_draw_wrapped(r, FX + 2 * BW + 64, BY + 8, FW - 2 * BW - 64,
                             kb_up ? "Type with the TV keyboard; its Enter key finishes."
                                   : "Select the field to open the TV keyboard again.",
                             28, UI_FONT_REGULAR, C(COL_TEXT_FAINT), 2);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    typing_pointer(win, false);
    drain_events();
    return result;
}

/* ── QR code ───────────────────────────────────────────────────────────────── */

static void draw_qr(SDL_Renderer *r, const char *text, int x, int y, int size)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    static char last[1024];
    static bool ok;
    if (strcmp(last, text) != 0) {
        snprintf(last, sizeof(last), "%s", text);
        ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    }
    set_color(r, 0xff, 0xff, 0xff, 0xff);
    fill_rounded_rect(r, x, y, size, size, 12);
    if (!ok) return;
    int n = qrcodegen_getSize(qr);
    int quiet = 3;
    int cell = size / (n + 2 * quiet);
    int ox = x + (size - cell * n) / 2, oy = y + (size - cell * n) / 2;
    set_color(r, 0x00, 0x00, 0x00, 0xff);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            if (qrcodegen_getModule(qr, i, j)) fill_rect(r, ox + i * cell, oy + j * cell, cell, cell);
}

/* ── PSN sign-in ───────────────────────────────────────────────────────────── */

typedef struct {
    char code[128];
    char *account, *online_id, *refresh;
    char err[256];
    bool ok;
} LoginJob;

static void login_job(void *p)
{
    LoginJob *j = p;
    j->ok = psn_login_with_code(j->code, &j->account, &j->online_id, &j->refresh, j->err, sizeof(j->err));
}

/* Signs in to a PSN account and adds it to the account list; returns its index or -1. */
int ui_psn_sign_in(SDL_Renderer *r, struct InputContext *in, AppConfig *cfg, const char *config_path)
{
    char msg[256] = "";
    int focus = 0;                               /* 0 Enter code, 1 Cancel */
    drain_events();
    for (;;) {
        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv == 0 || hv == 1) focus = hv;
        if (quit || ui_is_back_key(k)) { drain_events(); return -1; }
        if (k == SDLK_UP || k == SDLK_LEFT) focus = 0;
        if (k == SDLK_DOWN || k == SDLK_RIGHT) focus = 1;
        if (is_ok(k)) {
            if (focus == 1) { drain_events(); return -1; }
            char code[100] = "";
            if (text_input(r, in, "Sign-in code",
                           "Type the characters after \"code=\" in the address of the page you reached "
                           "after signing in, up to the \"&\" (upper and lower case matter). "
                           "Pasting the whole address works too.",
                           code, sizeof(code), false, false, 90) && code[0]) {
                LoginJob j;
                memset(&j, 0, sizeof(j));
                snprintf(j.code, sizeof(j.code), "%s", code);
                run_busy(r, "Signing in", login_job, &j);
                if (j.ok) {
                    int idx = config_add_psn_account(cfg, j.online_id, j.account, j.refresh);
                    free(j.account); free(j.online_id); free(j.refresh);
                    if (idx >= 0) {
                        config_select_console(cfg, cfg->current);   // refresh token may have changed
                        config_save(cfg, config_path);
                        return idx;
                    }
                    snprintf(msg, sizeof(msg), "Too many accounts: sign one out in App settings first.");
                } else {
                    snprintf(msg, sizeof(msg), "Sign-in failed: %s. Scan the code and sign in again for a new code.", j.err);
                }
            }
        }

        ui_draw_header(r, "Sign in to PlayStation Network", NULL);
        const int TOP = CONTENT_Y + 30, QS = 560;
        draw_qr(r, psn_login_url(), 120, TOP, QS);
        int tx = 120 + QS + 80, tw = SCREEN_W - tx - 120, y = TOP;
        ui_text_draw_wrapped(r, tx, y, tw,
                             "Linking a console needs the PSN account you use on it. The TV remembers "
                             "the account, so other consoles can use it too.",
                             32, UI_FONT_SEMIBOLD, C(COL_TEXT), 3);
        y += 120;
        y += ui_text_draw_wrapped(r, tx, y, tw,
                                  "1. Scan the code with your phone and sign in with the account you use on the console.",
                                  30, UI_FONT_REGULAR, C(COL_TEXT), 3) + 18;
        y += ui_text_draw_wrapped(r, tx, y, tw,
                                  "2. You end up on a page whose address starts with "
                                  "remoteplay.dl.playstation.net/remoteplay/redirect?code=  "
                                  "The page itself may look empty or show an error; that is fine.",
                                  30, UI_FONT_REGULAR, C(COL_TEXT), 4) + 18;
        y += ui_text_draw_wrapped(r, tx, y, tw,
                                  "3. Select Enter code and type what follows code= up to the & "
                                  "(about 6 characters, e.g. aB3xYz). Codes expire after a few minutes.",
                                  30, UI_FONT_REGULAR, C(COL_TEXT), 3) + 30;
        big_button(r, tx, y, 420, 100, "Enter code", focus == 0, true, true);
        big_button(r, tx + 450, y, 300, 100, "Cancel", focus == 1, true, false);
        ui_hit_add(0, tx, y, 420, 100);
        ui_hit_add(1, tx + 450, y, 300, 100);
        if (msg[0])
            ui_text_draw_wrapped(r, tx, y + 130, tw, msg, 28, UI_FONT_SEMIBOLD, C(COL_ERROR), 3);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
}

/* ── registration (PIN) ────────────────────────────────────────────────────── */

typedef struct {
    ChiakiLog *log;
    const char *host;
    bool ps5;
    int target;
    const char *account;
    uint32_t pin;
    RegistResult res;
    RegistState state;
} RegistJob;

static void regist_job(void *p)
{
    RegistJob *j = p;
    ConsoleRegist *reg = console_regist_start(j->log, j->host, j->ps5, j->target, j->account, j->pin);
    Uint32 t0 = SDL_GetTicks();
    j->state = REGIST_RUNNING;
    while ((j->state = console_regist_poll(reg, &j->res)) == REGIST_RUNNING && SDL_GetTicks() - t0 < 30000)
        SDL_Delay(100);
    console_regist_free(reg);
}

/* Consoles linked with account a, as "PS5, Living room" (or "no consoles"). */
static void account_users(const AppConfig *cfg, const char *account_id, char *out, size_t outlen)
{
    out[0] = '\0';
    for (int c = 0; c < cfg->n_consoles; c++) {
        const ConsoleConfig *cc = &cfg->consoles[c];
        if (!cc->psn_account_id_b64 || !account_id || strcmp(cc->psn_account_id_b64, account_id)) continue;
        size_t l = strlen(out);
        snprintf(out + l, outlen - l, "%s%s", l ? ", " : "", cc->name ? cc->name : "Console");
    }
    if (!out[0]) snprintf(out, outlen, "no consoles");
}

/* List rows used by the account screens. */
static void list_row(SDL_Renderer *r, int x, int y, int w, const char *title, const char *sub, bool f)
{
    if (f) { set_color(r, COL_TEXT); fill_rounded_rect(r, x - 4, y - 4, w + 8, 108, 18); }
    if (f) set_color(r, COL_ACCENT); else set_color(r, COL_CARD);
    fill_rounded_rect(r, x, y, w, 100, 14);
    ui_text_draw_fit(r, x + 30, y + 10, w - 60, title, 38, UI_FONT_SEMIBOLD, C(COL_TEXT));
    ui_text_draw_fit(r, x + 30, y + 58, w - 60, sub, 26, UI_FONT_REGULAR, f ? C(COL_TEXT) : C(COL_TEXT_DIM));
}

/* Which PSN account to link a console with: one already signed in on the TV,
 * or a new sign-in.  Returns the account index or -1. */
static int choose_psn_account(SDL_Renderer *r, struct InputContext *in, AppConfig *cfg,
                              const char *config_path, const char *preselect)
{
    if (cfg->n_psn_accounts == 0) return ui_psn_sign_in(r, in, cfg, config_path);
    int focus = config_find_psn_account(cfg, preselect);
    if (focus < 0) focus = 0;
    drain_events();
    for (;;) {
        int n = cfg->n_psn_accounts, items = n + 1;
        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv >= 0 && hv < items) focus = hv;
        if (quit || ui_is_back_key(k)) { drain_events(); return -1; }
        if (k == SDLK_UP && focus > 0) focus--;
        if (k == SDLK_DOWN && focus < items - 1) focus++;
        if (is_ok(k)) {
            if (focus < n) { drain_events(); return focus; }
            int idx = ui_psn_sign_in(r, in, cfg, config_path);
            if (idx >= 0) return idx;
            drain_events();
        }

        ui_draw_header(r, "Which PSN account?", NULL);
        const int LX = 120, W = SCREEN_W - 240;
        int y = CONTENT_Y + 24;
        ui_text_draw_wrapped(r, LX, y, W,
                             "Pick the PSN account you use on this console, or sign in with another one.",
                             30, UI_FONT_REGULAR, C(COL_TEXT_DIM), 2);
        y += 80;
        int first = focus >= 5 ? focus - 4 : 0;
        for (int i = first; i < n && i < first + 5; i++, y += 120) {
            char users[200], sub[240];
            account_users(cfg, cfg->psn_accounts[i].account_id_b64, users, sizeof(users));
            snprintf(sub, sizeof(sub), "Used by: %s", users);
            list_row(r, LX, y, W, cfg->psn_accounts[i].name, sub, focus == i);
            ui_hit_add(i, LX, y, W, 100);
        }
        big_button(r, LX, y + 10, 700, 96, "+  Sign in with another account", focus == n, true, false);
        ui_hit_add(n, LX, y + 10, 700, 96);
        ui_draw_help_footer(r, "The console is linked with this account; you can link it again with another one later.",
                            "Up/Down: move   OK: select   Back: cancel", NULL);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
}

static void icon_button(SDL_Renderer *r, int x, int y, int w, int h, const char *icon, bool focused);

/* Manage the signed-in PSN accounts (App settings).  Each account row has a
 * trash button at its end, like the consoles on the start screen. */
void ui_psn_accounts(SDL_Renderer *r, struct InputContext *in, AppConfig *cfg, const char *config_path)
{
    int focus = 0, col = 0;                      /* col 0 account, 1 trash */
    char msg[160] = "";
    drain_events();
    for (;;) {
        int n = cfg->n_psn_accounts, items = n + 1;
        if (focus >= items) focus = items - 1;
        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv >= 0 && hv / 2 < items) { focus = hv / 2; col = hv % 2; }
        if (quit || ui_is_back_key(k)) break;
        if (k == SDLK_UP && focus > 0) focus--;
        if (k == SDLK_DOWN && focus < items - 1) focus++;
        if (k == SDLK_RIGHT) col = 1;
        if (k == SDLK_LEFT) col = 0;
        if (focus >= n) col = 0;
        if (is_ok(k)) {
            msg[0] = '\0';
            if (focus < n && col == 0) {
                /* the account row itself: nothing to do */
            } else if (focus < n) {
                char q[160];
                snprintf(q, sizeof(q), "Sign out of %s?", cfg->psn_accounts[focus].name);
                ui_draw_header(r, "PSN accounts", NULL);
                if (confirm(r, in, q, "Consoles linked with it keep working. Cloud wakeup for them stops "
                            "until you sign in with this account again.", "Sign out", "Keep")) {
                    app_log_always("[UI] signed out of PSN account %s\n", cfg->psn_accounts[focus].name);
                    config_remove_psn_account(cfg, focus);
                    config_save(cfg, config_path);
                    snprintf(msg, sizeof(msg), "Signed out.");
                    col = 0;
                }
            } else if (ui_psn_sign_in(r, in, cfg, config_path) >= 0) {
                snprintf(msg, sizeof(msg), "Signed in.");
            }
            drain_events();
        }

        ui_draw_header(r, "PSN accounts", NULL);
        const int LX = 120, W = SCREEN_W - 240;
        int y = CONTENT_Y + 24;
        if (n == 0)
            ui_text_draw(r, LX, y + 10, "No account signed in. Adding a console signs in.", 32,
                         UI_FONT_SEMIBOLD, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
        if (n == 0) y += 80;
        const int IW = 112, RW = W - IW - 16;
        int first = focus >= 6 ? focus - 5 : 0;
        for (int i = first; i < n && i < first + 6; i++, y += 120) {
            char users[200], sub[260];
            account_users(cfg, cfg->psn_accounts[i].account_id_b64, users, sizeof(users));
            snprintf(sub, sizeof(sub), "Used by: %s  ·  cloud wakeup %s", users,
                     cfg->psn_accounts[i].refresh_token ? "available" : "not available");
            list_row(r, LX, y, RW, cfg->psn_accounts[i].name, sub, focus == i && col == 0);
            icon_button(r, LX + RW + 16, y, IW, 100, UI_ICON_TRASH, focus == i && col == 1);
            ui_hit_add(i * 2, LX, y, RW, 100);
            ui_hit_add(i * 2 + 1, LX + RW + 16, y, IW, 100);
        }
        big_button(r, LX, y + 10, 600, 96, "+  Sign in", focus == n, n < MAX_PSN_ACCOUNTS, false);
        ui_hit_add(n * 2, LX, y + 10, 600, 96);
        ui_draw_help_footer(r, focus >= n ? "Sign in with another PSN account."
                               : col == 1 ? "Sign this account out of the TV."
                                          : "A PSN account signed in on this TV.",
                            "Arrows: move   OK: select   Back: return", msg);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    drain_events();
}

/* Picks the PSN account, asks for the PIN and registers; fills out and
 * account_out (the account ID) on success. */
static bool pair_console(SDL_Renderer *r, struct InputContext *in, ChiakiLog *log, AppConfig *cfg,
                         const char *config_path, const char *host, bool ps5, int target,
                         const char *preselect, RegistResult *out, char *account_out, size_t account_cap)
{
    int ai = choose_psn_account(r, in, cfg, config_path, preselect);
    if (ai < 0) return false;
    char account[32];
    snprintf(account, sizeof(account), "%s", cfg->psn_accounts[ai].account_id_b64);
    char help[512];
    snprintf(help, sizeof(help),
             "On the %s: %s. Enter the 8-digit number shown there. "
             "The console must be on (not in rest mode) and on the same network as the TV.",
             ps5 ? "PS5" : "PS4",
             ps5 ? "Settings > System > Remote Play > Link Device"
                 : "Settings > Remote Play Connection Settings > Add Device");
    for (;;) {
        char pin[16] = "";
        if (!text_input(r, in, "Link with the console", help, pin, sizeof(pin), true, false, 8))
            return false;
        if (strlen(pin) != 8) {
            snprintf(help, sizeof(help), "The PIN has 8 digits. On the %s: %s.", ps5 ? "PS5" : "PS4",
                     ps5 ? "Settings > System > Remote Play > Link Device"
                         : "Settings > Remote Play Connection Settings > Add Device");
            continue;
        }
        RegistJob j;
        memset(&j, 0, sizeof(j));
        j.log = log; j.host = host; j.ps5 = ps5; j.target = target;
        j.account = account;
        j.pin = (uint32_t)strtoul(pin, NULL, 10);
        run_busy(r, "Linking", regist_job, &j);
        if (j.state == REGIST_OK) {
            *out = j.res;
            snprintf(account_out, account_cap, "%s", account);
            return true;
        }
        snprintf(help, sizeof(help),
                 "Linking failed. Check that the PIN is still shown on the console (it changes after a "
                 "few minutes) and that the console is on. On the %s: %s.", ps5 ? "PS5" : "PS4",
                 ps5 ? "Settings > System > Remote Play > Link Device"
                     : "Settings > Remote Play Connection Settings > Add Device");
    }
}

static void store_pairing(ConsoleConfig *c, const RegistResult *res, const char *account)
{
    config_set_str(&c->psn_account_id_b64, account);
    config_set_str(&c->registered_key_b64, res->regist_key_b64);
    config_set_str(&c->rp_key_b64, res->rp_key_b64);
    c->rp_key_type = res->rp_key_type;
    c->ps5 = res->ps5;
    char mac[24];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             res->mac[0], res->mac[1], res->mac[2], res->mac[3], res->mac[4], res->mac[5]);
    config_set_str(&c->mac, mac);
}

/* ── add a console ─────────────────────────────────────────────────────────── */

static const char *state_text(ConsoleState s)
{
    return s == CONSOLE_STATE_READY ? "On" : s == CONSOLE_STATE_STANDBY ? "Rest mode" : "Unknown";
}

/* Returns the new console's index, or -1. */
static int add_console(SDL_Renderer *r, struct InputContext *in, ChiakiLog *log, AppConfig *cfg,
                       const char *config_path, ConsoleDiscovery *disc)
{
    if (cfg->n_consoles >= MAX_CONSOLES) return -1;
    int focus = 0;
    char host[64] = "";
    bool ps5 = true;
    int target = 0;
    drain_events();

    /* 1. pick a console found on the network, or type its address */
    for (bool picked = false; !picked;) {
        DiscoveredConsole found[8];
        int n = console_discovery_snapshot(disc, found, 8);
        int items = n + 1;                       /* + "Enter address" */
        if (focus >= items) focus = items - 1;

        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv >= 0 && hv < items) focus = hv;
        if (quit || ui_is_back_key(k)) { drain_events(); return -1; }
        if (k == SDLK_UP && focus > 0) focus--;
        if (k == SDLK_DOWN && focus < items - 1) focus++;
        if (is_ok(k)) {
            if (focus < n) {
                snprintf(host, sizeof(host), "%s", found[focus].addr);
                ps5 = found[focus].ps5;
                target = found[focus].target;
                if (found[focus].state == CONSOLE_STATE_STANDBY) {
                    confirm(r, in, "The console is in rest mode",
                            "Turn it on fully (not rest mode), then select it again. Linking needs a PIN "
                            "shown on the console's screen.", "OK", "Back");
                    continue;
                }
                picked = true;
            } else {
                char ip[32] = "";
                if (!text_input(r, in, "Console address",
                                "Enter the console's IP address. On the PS5: Settings > Network > Connection "
                                "Status > View Connection Status (IPv4 Address).",
                                ip, sizeof(ip), true, true, 15))
                    continue;
                if (!valid_ipv4(ip)) continue;
                snprintf(host, sizeof(host), "%s", ip);
                ps5 = confirm(r, in, "Which console is it?", host, "PS5", "PS4");
                target = 0;
                picked = true;
            }
        }

        ui_draw_header(r, "Add a console", NULL);
        const int LX = 120, W = SCREEN_W - 240;
        int y = CONTENT_Y + 24;
        ui_text_draw_wrapped(r, LX, y, W,
                             "Turn on the console and keep it on the same network as the TV. Consoles found "
                             "on the network appear below.", 30, UI_FONT_REGULAR, C(COL_TEXT_DIM), 2);
        y += 100;
        if (n == 0)
            ui_text_draw(r, LX, y + 10, "Searching...", 34, UI_FONT_SEMIBOLD, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
        for (int i = 0; i < n; i++, y += 120) {
            bool f = focus == i;
            if (f) { set_color(r, COL_TEXT); fill_rounded_rect(r, LX - 4, y - 4, W + 8, 108, 18); }
            if (f) set_color(r, COL_ACCENT); else set_color(r, COL_CARD);
            fill_rounded_rect(r, LX, y, W, 100, 14);
            ui_hit_add(i, LX, y, W, 100);
            ui_text_draw(r, LX + 30, y + 10, found[i].name[0] ? found[i].name : found[i].addr, 38,
                         UI_FONT_SEMIBOLD, C(COL_TEXT), UI_ALIGN_LEFT);
            char sub[128];
            bool known = false;
            for (int c = 0; c < cfg->n_consoles; c++)
                if (cfg->consoles[c].host && !strcmp(cfg->consoles[c].host, found[i].addr)) known = true;
            snprintf(sub, sizeof(sub), "%s  ·  %s  ·  %s%s", found[i].ps5 ? "PS5" : "PS4", found[i].addr,
                     state_text(found[i].state), known ? "  ·  already added" : "");
            ui_text_draw(r, LX + 30, y + 58, sub, 26, UI_FONT_REGULAR, f ? C(COL_TEXT) : C(COL_TEXT_DIM), UI_ALIGN_LEFT);
        }
        if (n == 0) y += 80;
        big_button(r, LX, y + 10, 600, 96, "Enter address manually", focus == n, true, false);
        ui_hit_add(n, LX, y + 10, 600, 96);
        ui_draw_help_footer(r, "Select your console. Not listed? Enter its address manually.",
                            "Up/Down: move   OK: select   Back: cancel", NULL);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    /* 2. sign in (once) and link with the PIN; an address already in the list
     *    is linked again instead of being added twice */
    int idx = -1;
    for (int c = 0; c < cfg->n_consoles; c++)
        if (cfg->consoles[c].host && !strcmp(cfg->consoles[c].host, host)) idx = c;
    RegistResult res;
    char account[32];
    if (!pair_console(r, in, log, cfg, config_path, host, ps5, target,
                      idx >= 0 ? cfg->consoles[idx].psn_account_id_b64 : NULL, &res, account, sizeof(account)))
        return -1;
    if (idx < 0) idx = config_add_console(cfg, res.nickname, host, res.ps5);
    if (idx < 0) return -1;
    store_pairing(&cfg->consoles[idx], &res, account);
    config_select_console(cfg, idx);
    config_save(cfg, config_path);
    app_log_always("[HOME] added console \"%s\" at %s\n", cfg->consoles[idx].name, host);
    return idx;
}

/* ── console settings ──────────────────────────────────────────────────────── */

enum { CS_NAME, CS_ADDRESS, CS_ACCOUNT, CS_RES, CS_FPS, CS_BITRATE, CS_CODEC, CS_WAKE, CS_REST, CS_PAIR, CS_REMOVE, CS_BACK, CS_COUNT };

static const char *CS_HELP[CS_COUNT] = {
    [CS_NAME]    = "The name shown on the start screen.",
    [CS_ADDRESS] = "The console's IP address on your network.",
    [CS_ACCOUNT] = "The PSN account this console is linked with. OK links it again with this or another account (needs a new PIN).",
    [CS_RES]     = "Stream resolution. Remote Play streams at up to 1080p; the TV upscales it.",
    [CS_FPS]     = "60 fps feels much smoother; 30 fps uses half the bandwidth.",
    [CS_BITRATE] = "Higher looks sharper but needs a stronger network. Lower it if the picture stutters.",
    [CS_CODEC]   = "H.265 gives better quality per bit. Use H.265 HDR for HDR games on an HDR TV. PS4 always uses H.264.",
    [CS_WAKE]    = "Wake the console from rest mode when you connect.",
    [CS_REST]    = "Rest mode when you leave a stream (Back or Down, or holding Mute). Ask shows a choice each time.",
    [CS_PAIR]    = "Link this console again with a new PIN, e.g. after removing the TV from the console's device list.",
    [CS_REMOVE]  = "Remove this console from the TV.",
    [CS_BACK]    = "Go back to the start screen.",
};

static int find_idx(const int *opts, int n, int v, int def)
{
    for (int i = 0; i < n; i++) if (opts[i] == v) return i;
    return def;
}

/* Returns true if the console was removed. */
static bool console_settings(SDL_Renderer *r, struct InputContext *in, ChiakiLog *log, AppConfig *cfg,
                             const char *config_path, int idx)
{
    int focus = CS_RES;
    char msg[128] = "";
    drain_events();
    for (;;) {
        ConsoleConfig *c = &cfg->consoles[idx];
        int res_i = 1;
        for (int i = 0; i < N_RES; i++) if (RES_OPTIONS[i].w == c->video_width) res_i = i;
        int fps_i = find_idx(FPS_OPTIONS, N_FPS, c->video_fps, 1);
        int br_i = find_idx(BITRATE_OPTIONS, N_BITRATE, c->video_bitrate, 2);
        int codec_i = 1;
        for (int i = 0; i < N_CODEC; i++) if (c->video_codec && !strcmp(c->video_codec, CODEC_OPTIONS[i])) codec_i = i;

        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv >= 0 && hv < CS_COUNT) focus = hv;
        if (quit || ui_is_back_key(k)) break;
        if (k == SDLK_UP && focus > 0) focus--;
        if (k == SDLK_DOWN && focus < CS_COUNT - 1) focus++;
        bool fwd = k == SDLK_RIGHT || is_ok(k), back = k == SDLK_LEFT;
        if (fwd || back) {
            int d = fwd ? 1 : -1;
            bool changed = true;
            switch (focus) {
            case CS_NAME:
                if (is_ok(k)) {
                    char name[64];
                    snprintf(name, sizeof(name), "%s", c->name ? c->name : "");
                    if (text_input(r, in, "Console name", "Shown on the start screen.", name, sizeof(name), false, false, 30) && name[0])
                        config_set_str(&c->name, name);
                    else changed = false;
                } else changed = false;
                break;
            case CS_ADDRESS:
                if (is_ok(k)) {
                    char ip[32];
                    snprintf(ip, sizeof(ip), "%s", c->host ? c->host : "");
                    if (text_input(r, in, "Console address", "The console's IPv4 address.", ip, sizeof(ip), true, true, 15) && valid_ipv4(ip))
                        config_set_str(&c->host, ip);
                    else changed = false;
                } else changed = false;
                break;
            case CS_RES:     res_i = (res_i + d + N_RES) % N_RES;
                             c->video_width = RES_OPTIONS[res_i].w; c->video_height = RES_OPTIONS[res_i].h; break;
            case CS_FPS:     fps_i = (fps_i + d + N_FPS) % N_FPS; c->video_fps = FPS_OPTIONS[fps_i]; break;
            case CS_BITRATE: br_i = (br_i + d + N_BITRATE) % N_BITRATE; c->video_bitrate = BITRATE_OPTIONS[br_i]; break;
            case CS_CODEC:   codec_i = (codec_i + d + N_CODEC) % N_CODEC; config_set_str(&c->video_codec, CODEC_OPTIONS[codec_i]); break;
            case CS_WAKE:    c->wakeup = !c->wakeup; break;
            case CS_REST:    c->sleep_on_exit = (c->sleep_on_exit + (fwd ? 1 : 2)) % 3; break;
            case CS_ACCOUNT:
            case CS_PAIR:
                changed = false;
                if (is_ok(k)) {
                    RegistResult res;
                    char host[64], account[32];
                    snprintf(host, sizeof(host), "%s", c->host ? c->host : "");
                    if (pair_console(r, in, log, cfg, config_path, host, c->ps5, 0, c->psn_account_id_b64,
                                     &res, account, sizeof(account))) {
                        store_pairing(&cfg->consoles[idx], &res, account);
                        changed = true;
                        snprintf(msg, sizeof(msg), "Linked again.");
                    }
                }
                break;
            case CS_REMOVE:
                changed = false;
                if (is_ok(k)) {
                    char q[160];
                    snprintf(q, sizeof(q), "Remove \"%s\"?", c->name ? c->name : "console");
                    if (confirm(r, in, q, "You can add it again later; that needs a new PIN from the console.",
                                "Remove", "Keep")) {
                        config_remove_console(cfg, idx);
                        config_save(cfg, config_path);
                        drain_events();
                        return true;
                    }
                }
                break;
            case CS_BACK:
                changed = false;
                if (is_ok(k)) goto done;
                break;
            default: changed = false; break;
            }
            if (changed) {
                if (cfg->current == idx) config_select_console(cfg, idx);
                if (!msg[0] || strcmp(msg, "Linked again.") != 0)
                    snprintf(msg, sizeof(msg), "%s", config_save(cfg, config_path) ? "Saved." : "Failed to write config.json");
                else
                    config_save(cfg, config_path);
            }
        }

        c = &cfg->consoles[idx];
        char note[96];
        snprintf(note, sizeof(note), "%s  ·  %s", c->ps5 ? "PlayStation 5" : "PlayStation 4",
                 console_registered(c) ? "linked" : "not linked");
        char title[96];
        snprintf(title, sizeof(title), "%s", c->name ? c->name : "Console");
        ui_draw_header(r, title, note);

        const int ROW_H = 58, ROW_GAP = 8, GROUP_GAP = 18;
        const int COL_W = (SCREEN_W - 60 * 2 - 40) / 2;
        const int LX = 60, RX = LX + COL_W + 40;
        char v[64];
#define GROUP(x, y, name) do { set_color(r, COL_ACCENT); draw_text(r, (x) + 4, (y), name, 2); (y) += 34; } while (0)
#define ROW(x, y, id, label, value) do { \
        ui_draw_option_row(r, (x), (y), COL_W, ROW_H, label, value, focus == (id)); \
        ui_hit_add((id), (x), (y), COL_W, ROW_H); \
        (y) += ROW_H + ROW_GAP; } while (0)
        int y = CONTENT_Y + 6;
        GROUP(LX, y, "CONSOLE");
        ROW(LX, y, CS_NAME, "Name", c->name);
        ROW(LX, y, CS_ADDRESS, "Address", c->host ? c->host : "-");
        ROW(LX, y, CS_ACCOUNT, "PSN account", config_psn_account_name(cfg, c->psn_account_id_b64));
        y += GROUP_GAP;
        GROUP(LX, y, "VIDEO");
        snprintf(v, sizeof(v), "%s  (%dx%d)", RES_OPTIONS[res_i].label, RES_OPTIONS[res_i].w, RES_OPTIONS[res_i].h);
        ROW(LX, y, CS_RES, "Resolution", v);
        snprintf(v, sizeof(v), "%d fps", FPS_OPTIONS[fps_i]);
        ROW(LX, y, CS_FPS, "Frame rate", v);
        snprintf(v, sizeof(v), "%d Mbit/s", BITRATE_OPTIONS[br_i] / 1000);
        ROW(LX, y, CS_BITRATE, "Bitrate", v);
        ROW(LX, y, CS_CODEC, "Codec", CODEC_LABELS[codec_i]);

        y = CONTENT_Y + 6;
        GROUP(RX, y, "POWER");
        ROW(RX, y, CS_WAKE, "Wake console on connect", on_off(c->wakeup));
        ROW(RX, y, CS_REST, "Rest mode on exit", SLEEP_LABELS[c->sleep_on_exit % 3]);
        y += GROUP_GAP;
        GROUP(RX, y, "MANAGE");
        draw_button(r, RX, y, COL_W, ROW_H + 6, "Link again", focus == CS_PAIR, true);
        ui_hit_add(CS_PAIR, RX, y, COL_W, ROW_H + 6); y += ROW_H + 6 + ROW_GAP;
        draw_button(r, RX, y, COL_W, ROW_H + 6, "Remove console", focus == CS_REMOVE, true);
        ui_hit_add(CS_REMOVE, RX, y, COL_W, ROW_H + 6); y += ROW_H + 6 + ROW_GAP + GROUP_GAP;
        draw_button(r, RX, y, COL_W, ROW_H + 6, "Back", focus == CS_BACK, true);
        ui_hit_add(CS_BACK, RX, y, COL_W, ROW_H + 6);
#undef ROW
#undef GROUP
        ui_draw_help_footer(r, CS_HELP[focus], "Up/Down: move   Left/Right/OK: change   Back: return", msg);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
done:
    drain_events();
    return false;
}

/* ── start screen ──────────────────────────────────────────────────────────── */

/* Square button with a Font Awesome icon (assets/fa-solid-900.ttf). */
static void icon_button(SDL_Renderer *r, int x, int y, int w, int h, const char *icon, bool focused)
{
    if (focused) { set_color(r, COL_TEXT); fill_rounded_rect(r, x - 4, y - 4, w + 8, h + 8, 16); }
    if (focused) set_color(r, COL_ACCENT); else set_color(r, COL_CARD);
    fill_rounded_rect(r, x, y, w, h, 14);
    const int px = 40;
    ui_text_draw(r, x + w / 2, y + (h - ui_text_line_height(px)) / 2, icon, px, UI_FONT_ICON,
                 focused ? C(COL_TEXT) : C(COL_TEXT_DIM), UI_ALIGN_CENTER);
}

enum { HC_CARD = 0, HC_SETTINGS, HC_REMOVE };

static void draw_controllers(SDL_Renderer *r, struct InputContext *in, int x, int y, int w)
{
    ui_text_draw(r, x, y, "Controllers", 34, UI_FONT_BOLD, C(COL_TEXT), UI_ALIGN_LEFT);
    y += 54;
    InputPadInfo pads[4];
    int n = in ? input_get_pads(in, pads, 4) : 0;
    if (n == 0)
        ui_text_draw_wrapped(r, x, y, w, "None connected. Turn a controller on, or plug it in by USB.",
                             28, UI_FONT_REGULAR, C(COL_WARNING), 2);
    for (int i = 0; i < n && i < 3; i++) {
        if (pads[i].outputs_active) set_color(r, COL_SUCCESS); else set_color(r, COL_WARNING);
        fill_rounded_rect(r, x, y + 12, 14, 14, 7);
        ui_text_draw_fit(r, x + 30, y, w - 30, pads[i].name, 30, UI_FONT_SEMIBOLD, C(COL_TEXT));
        y += 40;
        char line[160];
        snprintf(line, sizeof(line), "%s  -  %s", pads[i].connection, pads[i].outputs);
        y += ui_text_draw_wrapped(r, x + 30, y, w - 30, line, 26, UI_FONT_REGULAR, C(COL_TEXT_DIM), 2) + 14;
    }
}

UIResult ui_run_home(SDL_Renderer *r, AppConfig *cfg, const char *config_path,
                     ChiakiLog *log, struct InputContext *in)
{
    ui_text_init(r);
    ConsoleDiscovery *disc = console_discovery_start(log);
    int row = cfg->current >= 0 ? cfg->current : 0, col = HC_CARD;
    char msg[256];
    snprintf(msg, sizeof(msg), "%s", ui_take_launcher_notice());
    UIResult result = UI_RESULT_QUIT;
    drain_events();

    for (;;) {
        const int n = cfg->n_consoles;
        const int ROW_ADD = n, ROW_BOTTOM = n + 1;
        if (row > ROW_BOTTOM) row = ROW_BOTTOM;
        DiscoveredConsole found[8];
        int nf = console_discovery_snapshot(disc, found, 8);

        bool quit = false;
        SDL_Keycode k = poll_key(in, &quit);
        int hv = ui_pointer_hover();
        if (hv >= 0 && hv / 4 <= ROW_BOTTOM) { row = hv / 4; col = hv % 4; }
        if (quit) { result = UI_RESULT_QUIT; break; }           /* Home button: leave at once */
        if (ui_is_back_key(k)) {
            /* the only screen where Back leaves the app: ask first */
            if (confirm(r, in, "Exit Chiaki-webOS?", "", "Exit", "Stay")) { result = UI_RESULT_QUIT; break; }
            drain_events();
            continue;
        }
        if (k == SDLK_UP && row > 0) { row--; if (row >= n) col = 0; }
        if (k == SDLK_DOWN && row < ROW_BOTTOM) { row++; if (row >= n) col = 0; }
        int max_col = row < n ? HC_REMOVE : row == ROW_ADD ? 0 : 1;
        if (k == SDLK_LEFT && col > 0) col--;
        if (k == SDLK_RIGHT && col < max_col) col++;
        if (col > max_col) col = max_col;

        if (is_ok(k)) {
            msg[0] = '\0';
            if (row < n && col == HC_CARD) {
                ConsoleConfig *c = &cfg->consoles[row];
                if (!console_registered(c)) {
                    RegistResult res;
                    char account[32];
                    if (c->host && pair_console(r, in, log, cfg, config_path, c->host, c->ps5, 0,
                                                c->psn_account_id_b64, &res, account, sizeof(account))) {
                        store_pairing(c, &res, account);
                        config_save(cfg, config_path);
                    }
                } else if (!c->host || !valid_ipv4(c->host)) {
                    snprintf(msg, sizeof(msg), "Set the console's address in its settings first.");
                } else {
                    config_select_console(cfg, row);
                    config_save(cfg, config_path);          // remembers the last console
                    result = UI_RESULT_CONNECT;
                    break;
                }
            } else if (row < n && col == HC_SETTINGS) {
                console_settings(r, in, log, cfg, config_path, row);
            } else if (row < n && col == HC_REMOVE) {
                char q[160];
                snprintf(q, sizeof(q), "Remove \"%s\"?", cfg->consoles[row].name);
                if (confirm(r, in, q, "You can add it again later; that needs a new PIN from the console.",
                            "Remove", "Keep")) {
                    config_remove_console(cfg, row);
                    config_save(cfg, config_path);
                    col = HC_CARD;
                }
            } else if (row == ROW_ADD) {
                int idx = add_console(r, in, log, cfg, config_path, disc);
                if (idx >= 0) { row = idx; col = HC_CARD; snprintf(msg, sizeof(msg), "Console added. Press OK to connect."); }
            } else if (row == ROW_BOTTOM && col == 0) {
                ui_run_settings(r, cfg, config_path, in);
            } else if (row == ROW_BOTTOM && col == 1) {
                ui_run_padtest(r, in);
            }
            drain_events();
            continue;
        }

        /* ── draw ── (pointer ids: row * 4 + col) */
        ui_hit_begin();
        const int M = 64, LW = 1180, RX = M + LW + 56, RW = SCREEN_W - RX - M;
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        set_color(r, COL_BG);
        fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
        set_color(r, COL_ACCENT);
        fill_rect(r, 0, 0, SCREEN_W, 6);
        ui_draw_logo(r, M, 26, 76);
        const int TX = M + 76 + 22;
        ui_text_draw(r, TX, 28, "Chiaki-webOS", 60, UI_FONT_BOLD, C(COL_TEXT), UI_ALIGN_LEFT);
        ui_text_draw(r, TX + ui_text_width("Chiaki-webOS", 60, UI_FONT_BOLD) + 28, 50,
                     "PlayStation Remote Play for LG TVs", 32, UI_FONT_REGULAR, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
        ui_text_draw(r, SCREEN_W - M, 50, "v" APP_VERSION " (" APP_GIT ")", 24, UI_FONT_REGULAR,
                     C(COL_TEXT_FAINT), UI_ALIGN_RIGHT);

        int y = 150;
        ui_text_draw(r, M, y, "Consoles", 30, UI_FONT_SEMIBOLD, C(COL_TEXT_DIM), UI_ALIGN_LEFT);
        y += 50;
        const int CH = 112, GAP = 16, BW = 200, IW = 112, CW = LW - (BW + 16) - (IW + 16);
        /* scroll so the focused row stays visible */
        int visible = 5, first = 0;
        if (row < n && row >= visible) first = row - visible + 1;
        if (row >= n && n > visible) first = n - visible + 1;
        for (int i = first; i < n && i < first + visible; i++, y += CH + GAP) {
            ConsoleConfig *c = &cfg->consoles[i];
            ConsoleState st = CONSOLE_STATE_UNKNOWN;
            bool seen = false;
            for (int j = 0; j < nf; j++)
                if (c->host && !strcmp(c->host, found[j].addr)) { st = found[j].state; seen = true; }
            bool fc = row == i && col == HC_CARD;
            if (fc) { set_color(r, COL_TEXT); fill_rounded_rect(r, M - 4, y - 4, CW + 8, CH + 8, 20); }
            if (fc) set_color(r, COL_ACCENT); else set_color(r, COL_CARD);
            fill_rounded_rect(r, M, y, CW, CH, 16);
            if (st == CONSOLE_STATE_READY) set_color(r, COL_SUCCESS);
            else if (st == CONSOLE_STATE_STANDBY) set_color(r, COL_WARNING);
            else set_color(r, COL_TEXT_FAINT);
            fill_rounded_rect(r, M + 28, y + CH / 2 - 9, 18, 18, 9);
            ui_text_draw_fit(r, M + 66, y + 14, CW - 90, c->name ? c->name : "Console", 38, UI_FONT_SEMIBOLD, C(COL_TEXT));
            char sub[160];
            snprintf(sub, sizeof(sub), "%s  ·  %s  ·  %s", c->ps5 ? "PS5" : "PS4", c->host ? c->host : "no address",
                     !console_registered(c) ? "not linked - press OK to link"
                     : seen ? (st == CONSOLE_STATE_READY ? "On" : st == CONSOLE_STATE_STANDBY ? "Rest mode" : "Found")
                            : "Not found on the network");
            ui_text_draw_fit(r, M + 66, y + 66, CW - 90, sub, 26, UI_FONT_REGULAR, fc ? C(COL_TEXT) : C(COL_TEXT_DIM));
            big_button(r, M + CW + 16, y + 6, BW, CH - 12, "Settings", row == i && col == HC_SETTINGS, true, false);
            icon_button(r, M + CW + 16 + BW + 16, y + 6, IW, CH - 12, UI_ICON_TRASH, row == i && col == HC_REMOVE);
            ui_hit_add(i * 4 + HC_CARD, M, y, CW, CH);
            ui_hit_add(i * 4 + HC_SETTINGS, M + CW + 16, y + 6, BW, CH - 12);
            ui_hit_add(i * 4 + HC_REMOVE, M + CW + 16 + BW + 16, y + 6, IW, CH - 12);
        }
        /* + Add console */
        {
            bool f = row == ROW_ADD;
            if (n >= MAX_CONSOLES) {
                ui_text_draw(r, M, y + 20, "Console limit reached", 28, UI_FONT_REGULAR, C(COL_TEXT_FAINT), UI_ALIGN_LEFT);
            } else {
                if (f) { set_color(r, COL_TEXT); fill_rounded_rect(r, M - 4, y - 4, CW + 8, 96 + 8, 20); }
                if (f) set_color(r, COL_ACCENT); else set_color(r, COL_PANEL);
                fill_rounded_rect(r, M, y, CW, 96, 16);
                ui_hit_add(ROW_ADD * 4, M, y, CW, 96);
                set_color(r, COL_BORDER);
                if (!f) stroke_rounded_rect(r, M, y, CW, 96, 16);
                ui_text_draw(r, M + CW / 2, y + (96 - ui_text_line_height(36)) / 2,
                             n == 0 ? "+  Add your console" : "+  Add console", 36, UI_FONT_SEMIBOLD,
                             C(COL_TEXT), UI_ALIGN_CENTER);
            }
            y += 96 + GAP;
        }
        /* bottom row */
        const int BY = SCREEN_H - 76 - 130;
        big_button(r, M, BY, 380, 100, "App settings", row == ROW_BOTTOM && col == 0, true, false);
        big_button(r, M + 404, BY, 380, 100, "Controller test", row == ROW_BOTTOM && col == 1, true, false);
        ui_hit_add(ROW_BOTTOM * 4, M, BY, 380, 100);
        ui_hit_add(ROW_BOTTOM * 4 + 1, M + 404, BY, 380, 100);
        if (msg[0]) {
            bool bad = strstr(msg, "ailed") || strstr(msg, "first");
            ui_text_draw_wrapped(r, M + 820, BY + 10, LW - 820, msg, 28, UI_FONT_SEMIBOLD,
                                 bad ? C(COL_ERROR) : C(COL_SUCCESS), 3);
        }

        /* right panel */
        panel(r, RX, 150, RW, SCREEN_H - 76 - 30 - 150);
        int ry = 180;
        int shown = row < n ? row : (cfg->current >= 0 && cfg->current < n ? cfg->current : -1);
        if (shown >= 0) {
            ConsoleConfig *c = &cfg->consoles[shown];
            ui_text_draw_fit(r, RX + 40, ry, RW - 80, c->name, 40, UI_FONT_BOLD, C(COL_TEXT));
            ry += 64;
            char line[160];
            snprintf(line, sizeof(line), "%dp  ·  %d fps  ·  %d Mbit/s  ·  %s", c->video_height, c->video_fps,
                     c->video_bitrate / 1000, c->video_codec && !strcmp(c->video_codec, "h265_hdr") ? "HDR"
                     : c->video_codec && !strcmp(c->video_codec, "h264") ? "H.264" : "H.265");
            ui_text_draw_fit(r, RX + 40, ry, RW - 80, line, 26, UI_FONT_REGULAR, C(COL_TEXT_DIM));
            ry += 40;
            snprintf(line, sizeof(line), "Wake on connect: %s", on_off(c->wakeup));
            ui_text_draw_fit(r, RX + 40, ry, RW - 80, line, 26, UI_FONT_REGULAR, C(COL_TEXT_DIM));
            ry += 36;
            snprintf(line, sizeof(line), "Rest mode on exit: %s", SLEEP_LABELS[c->sleep_on_exit % 3]);
            ui_text_draw_fit(r, RX + 40, ry, RW - 80, line, 26, UI_FONT_REGULAR, C(COL_TEXT_DIM));
            ry += 60;
        } else if (n == 0) {
            ui_text_draw(r, RX + 40, ry, "Welcome", 40, UI_FONT_BOLD, C(COL_TEXT), UI_ALIGN_LEFT);
            ry += 64;
            ry += ui_text_draw_wrapped(r, RX + 40, ry, RW - 80,
                                       "Add your PS5 or PS4: the TV finds it on your network and links with "
                                       "a PIN shown on the console.", 28, UI_FONT_REGULAR, C(COL_TEXT_DIM), 4) + 30;
        } else {
            ry += 4;
        }
        set_color(r, COL_BORDER_DIM);
        fill_rect(r, RX + 40, ry, RW - 80, 2);
        draw_controllers(r, in, RX + 40, ry + 24, RW - 80);

        /* footer */
        set_color(r, COL_PANEL);
        fill_rect(r, 0, SCREEN_H - 76, SCREEN_W, 76);
        ui_text_draw(r, SCREEN_W / 2, SCREEN_H - 76 + (76 - ui_text_line_height(28)) / 2,
                     "Arrows / D-pad / pointer: move    OK / Cross: select    Back / Circle: exit",
                     28, UI_FONT_REGULAR, C(COL_TEXT_DIM), UI_ALIGN_CENTER);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    console_discovery_stop(disc);
    drain_events();
    return result;
}
