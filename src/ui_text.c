#include "ui_text.h"
#include "app_log.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── SDL2_ttf, loaded at runtime ──────────────────────────────────────────── */

typedef struct _TTF_Font TTF_Font;

static struct {
    void *lib;
    int        (*Init)(void);
    void       (*Quit)(void);
    TTF_Font  *(*OpenFont)(const char *file, int ptsize);
    void       (*CloseFont)(TTF_Font *font);
    SDL_Surface*(*RenderUTF8_Blended)(TTF_Font *font, const char *text, SDL_Color fg);
    int        (*SizeUTF8)(TTF_Font *font, const char *text, int *w, int *h);
    int        (*FontHeight)(const TTF_Font *font);
} ttf;

static bool          g_inited;
static bool          g_have_ttf;
static SDL_Renderer *g_renderer;
static const char   *g_font_files[UI_FONT_COUNT];

static const char *FONT_CANDIDATES[UI_FONT_COUNT][3] = {
    [UI_FONT_REGULAR]  = { "/usr/share/fonts/LG_Smart_UI-Regular.ttf",
                           "/usr/share/fonts/DroidSans.ttf", NULL },
    [UI_FONT_SEMIBOLD] = { "/usr/share/fonts/LG_Smart_UI-SemiBold.ttf",
                           "/usr/share/fonts/LG_Smart_UI-Bold.ttf",
                           "/usr/share/fonts/DroidSans.ttf" },
    [UI_FONT_BOLD]     = { "/usr/share/fonts/LG_Smart_UI-Bold.ttf",
                           "/usr/share/fonts/LG_Smart_UI-SemiBold.ttf",
                           "/usr/share/fonts/DroidSans.ttf" },
};

/* Open fonts, one per (weight, size). */
#define MAX_FONTS 24
static struct { UiFontWeight w; int px; TTF_Font *f; } g_fonts[MAX_FONTS];
static int g_nfonts;

static TTF_Font *get_font(UiFontWeight w, int px)
{
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].w == w && g_fonts[i].px == px) return g_fonts[i].f;
    if (!g_font_files[w]) return NULL;
    TTF_Font *f = ttf.OpenFont(g_font_files[w], px);
    if (!f) return NULL;
    if (g_nfonts == MAX_FONTS) {                 /* full: drop the oldest */
        ttf.CloseFont(g_fonts[0].f);
        memmove(&g_fonts[0], &g_fonts[1], sizeof(g_fonts[0]) * (MAX_FONTS - 1));
        g_nfonts--;
    }
    g_fonts[g_nfonts].w = w; g_fonts[g_nfonts].px = px; g_fonts[g_nfonts].f = f;
    g_nfonts++;
    return f;
}

/* Rendered-string cache (white text; tinted with colour/alpha mod per draw). */
#define CACHE_SIZE 256
typedef struct {
    char        *text;
    uint32_t     hash;
    int          px;
    UiFontWeight w;
    SDL_Texture *tex;
    int          tw, th;
    uint32_t     used;
} CacheEntry;
static CacheEntry g_cache[CACHE_SIZE];
static uint32_t   g_use_counter;

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static CacheEntry *cache_get(const char *s, int px, UiFontWeight w)
{
    uint32_t h = hash_str(s);
    CacheEntry *victim = &g_cache[0];
    for (int i = 0; i < CACHE_SIZE; i++) {
        CacheEntry *e = &g_cache[i];
        if (e->text && e->hash == h && e->px == px && e->w == w && !strcmp(e->text, s)) {
            e->used = ++g_use_counter;
            return e;
        }
        if (!e->text) { victim = e; break; }
        if (e->used < victim->used) victim = e;
    }
    TTF_Font *f = get_font(w, px);
    if (!f) return NULL;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf = ttf.RenderUTF8_Blended(f, s, white);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_renderer, surf);
    int sw = surf->w, sh = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    if (victim->tex) SDL_DestroyTexture(victim->tex);
    free(victim->text);
    victim->text = strdup(s);
    victim->hash = h; victim->px = px; victim->w = w;
    victim->tex = tex; victim->tw = sw; victim->th = sh;
    victim->used = ++g_use_counter;
    return victim;
}

/* ── Bitmap fallback (5×7) ────────────────────────────────────────────────── */

static const uint8_t FONT_DATA[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14},{0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00},{0x08,0x2a,0x1c,0x2a,0x08},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7f,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

_Static_assert(sizeof(FONT_DATA) / sizeof(FONT_DATA[0]) == 95, "bitmap font must cover 0x20-0x7e");

static int bm_scale(int px) { int s = (px + 5) / 10; return s < 1 ? 1 : s; }

static int bm_width(const char *s, int px)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n'; p++)
        if ((*p & 0xc0) != 0x80) n++;            /* count UTF-8 code points */
    return n * 6 * bm_scale(px);
}

static void bm_draw(SDL_Renderer *r, int x, int y, const char *s, int px)
{
    int sc = bm_scale(px);
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n'; p++) {
        if ((*p & 0xc0) == 0x80) continue;
        uint8_t ch = (*p >= 0x20 && *p <= 0x7e) ? *p : '?';
        const uint8_t *col = FONT_DATA[ch - 0x20];
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++)
                if (col[cx] & (1 << cy)) {
                    SDL_Rect rc = { x + cx * sc, y + cy * sc, sc, sc };
                    SDL_RenderFillRect(r, &rc);
                }
        x += 6 * sc;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool ui_text_init(SDL_Renderer *r)
{
    g_renderer = r;
    if (g_inited) return g_have_ttf;
    g_inited = true;

    const char *libs[] = { "libSDL2_ttf-2.0.so.0", "libSDL2_ttf.so", NULL };
    for (int i = 0; libs[i] && !ttf.lib; i++) ttf.lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
    if (!ttf.lib) {
        app_log_always("[UI] SDL2_ttf not available (%s) - using bitmap font\n", dlerror());
        return false;
    }
#define LOAD(sym) *(void **)&ttf.sym = dlsym(ttf.lib, "TTF_" #sym)
    LOAD(Init); LOAD(Quit); LOAD(OpenFont); LOAD(CloseFont);
    LOAD(RenderUTF8_Blended); LOAD(SizeUTF8); LOAD(FontHeight);
#undef LOAD
    if (!ttf.Init || !ttf.OpenFont || !ttf.CloseFont || !ttf.RenderUTF8_Blended || !ttf.SizeUTF8 ||
        ttf.Init() != 0) {
        app_log_always("[UI] SDL2_ttf unusable - using bitmap font\n");
        dlclose(ttf.lib); ttf.lib = NULL;
        return false;
    }
    for (int w = 0; w < UI_FONT_COUNT; w++)
        for (int i = 0; i < 3 && !g_font_files[w]; i++)
            if (FONT_CANDIDATES[w][i] && access(FONT_CANDIDATES[w][i], R_OK) == 0)
                g_font_files[w] = FONT_CANDIDATES[w][i];
    if (!g_font_files[UI_FONT_REGULAR] || !get_font(UI_FONT_REGULAR, 32)) {
        app_log_always("[UI] no usable TrueType font in /usr/share/fonts - using bitmap font\n");
        return false;
    }
    for (int w = 0; w < UI_FONT_COUNT; w++)
        if (!g_font_files[w]) g_font_files[w] = g_font_files[UI_FONT_REGULAR];
    g_have_ttf = true;
    app_log_always("[UI] text: %s\n", g_font_files[UI_FONT_REGULAR]);
    return true;
}

void ui_text_shutdown(void)
{
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (g_cache[i].tex) SDL_DestroyTexture(g_cache[i].tex);
        free(g_cache[i].text);
        memset(&g_cache[i], 0, sizeof(g_cache[i]));
    }
    if (g_have_ttf) {
        for (int i = 0; i < g_nfonts; i++) ttf.CloseFont(g_fonts[i].f);
        g_nfonts = 0;
    }
}

bool ui_text_has_ttf(void) { return g_have_ttf; }

int ui_text_line_height(int px) { return px + px / 4; }

int ui_text_width(const char *s, int px, UiFontWeight w)
{
    if (!s || !s[0]) return 0;
    if (g_have_ttf) {
        TTF_Font *f = get_font(w, px);
        int tw = 0, th = 0;
        if (f && ttf.SizeUTF8(f, s, &tw, &th) == 0) return tw;
    }
    return bm_width(s, px);
}

void ui_text_draw(SDL_Renderer *r, int x, int y, const char *s, int px, UiFontWeight w,
                  SDL_Color c, UiAlign align)
{
    if (!s || !s[0]) return;
    int lh = ui_text_line_height(px);
    if (g_have_ttf) {
        g_renderer = r;
        CacheEntry *e = cache_get(s, px, w);
        if (e) {
            int dx = align == UI_ALIGN_CENTER ? x - e->tw / 2 : align == UI_ALIGN_RIGHT ? x - e->tw : x;
            SDL_Rect dst = { dx, y + (lh - e->th) / 2, e->tw, e->th };
            SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(e->tex, c.a);
            SDL_RenderCopy(r, e->tex, NULL, &dst);
            return;
        }
    }
    int tw = bm_width(s, px);
    int dx = align == UI_ALIGN_CENTER ? x - tw / 2 : align == UI_ALIGN_RIGHT ? x - tw : x;
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(r, &old);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    bm_draw(r, dx, y + (lh - 7 * bm_scale(px)) / 2, s, px);
    SDL_SetRenderDrawBlendMode(r, old);
}

/* Shared word-wrap: calls `emit` for each line (may be NULL to just count). */
static int wrap(int max_w, const char *s, int px, UiFontWeight w, int max_lines,
                void (*emit)(void *, int, const char *), void *ud)
{
    char line[512], test[512];
    int lines = 0;
    line[0] = '\0';
    const char *p = s;
    while (p && *p) {
        /* next word (up to space or newline) */
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') e++;
        size_t wl = (size_t)(e - p);
        if (wl >= sizeof(test) - 2) wl = sizeof(test) - 2;
        snprintf(test, sizeof(test), "%s%s%.*s", line, line[0] ? " " : "", (int)wl, p);
        if (line[0] && ui_text_width(test, px, w) > max_w) {
            if (max_lines > 0 && lines + 1 == max_lines) {
                size_t l = strlen(line);
                if (l + 4 < sizeof(line)) strcat(line, "...");
                if (emit) emit(ud, lines, line);
                return lines + 1;
            }
            if (emit) emit(ud, lines, line);
            lines++;
            snprintf(line, sizeof(line), "%.*s", (int)wl, p);
        } else {
            snprintf(line, sizeof(line), "%s", test);
        }
        p = e;
        if (*p == ' ') p++;
        else if (*p == '\n') {
            if (emit) emit(ud, lines, line);
            lines++;
            line[0] = '\0';
            p++;
            if (max_lines > 0 && lines >= max_lines) return lines;
        }
    }
    if (line[0]) { if (emit) emit(ud, lines, line); lines++; }
    return lines;
}

typedef struct { SDL_Renderer *r; int x, y, px; UiFontWeight w; SDL_Color c; } WrapCtx;

static void wrap_emit(void *ud, int idx, const char *line)
{
    WrapCtx *wc = ud;
    ui_text_draw(wc->r, wc->x, wc->y + idx * ui_text_line_height(wc->px), line, wc->px, wc->w, wc->c,
                 UI_ALIGN_LEFT);
}

int ui_text_draw_wrapped(SDL_Renderer *r, int x, int y, int max_w, const char *s, int px,
                         UiFontWeight w, SDL_Color c, int max_lines)
{
    if (!s || !s[0]) return 0;
    WrapCtx wc = { r, x, y, px, w, c };
    return wrap(max_w, s, px, w, max_lines, wrap_emit, &wc) * ui_text_line_height(px);
}

int ui_text_measure_wrapped(int max_w, const char *s, int px, UiFontWeight w)
{
    if (!s || !s[0]) return 0;
    return wrap(max_w, s, px, w, 0, NULL, NULL) * ui_text_line_height(px);
}
