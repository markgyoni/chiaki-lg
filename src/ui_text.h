#pragma once
/* TV-readable text for the launcher UI.
 *
 * Uses SDL2_ttf with LG's own UI fonts when available.  Neither is in the
 * SDK sysroot, so libSDL2_ttf is loaded at runtime with dlopen() (every
 * webOS TV ships it) and the fonts come from /usr/share/fonts.  If either is
 * missing, text falls back to a scaled bitmap font, so the UI always works.
 *
 * Rendered strings are cached as textures (white glyphs, tinted per draw). */

#include <SDL2/SDL.h>
#include <stdbool.h>

typedef enum { UI_FONT_REGULAR = 0, UI_FONT_SEMIBOLD, UI_FONT_BOLD, UI_FONT_COUNT } UiFontWeight;

typedef enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER, UI_ALIGN_RIGHT } UiAlign;

/* Call once a renderer exists (idempotent).  Returns true if TrueType fonts
 * are in use, false for the bitmap fallback. */
bool ui_text_init(SDL_Renderer *r);
void ui_text_shutdown(void);
bool ui_text_has_ttf(void);

/* Width in pixels of one line of text at size px (cap height ~0.7 px). */
int  ui_text_width(const char *s, int px, UiFontWeight w);
/* Line height for size px. */
int  ui_text_line_height(int px);

/* Draw one line; (x, y) is the top of the line box, x is the anchor for align. */
void ui_text_draw(SDL_Renderer *r, int x, int y, const char *s, int px, UiFontWeight w,
                  SDL_Color c, UiAlign align);

/* Draw text wrapped to max_w (breaks at spaces and '\n').  Returns the
 * height used.  If max_lines > 0, stops after that many lines ("..." added). */
int  ui_text_draw_wrapped(SDL_Renderer *r, int x, int y, int max_w, const char *s, int px,
                          UiFontWeight w, SDL_Color c, int max_lines);

/* Height wrapped text would take, without drawing. */
int  ui_text_measure_wrapped(int max_w, const char *s, int px, UiFontWeight w);
