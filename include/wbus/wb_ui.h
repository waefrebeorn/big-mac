#ifndef WUBUS_WB_UI_H
#define WUBUS_WB_UI_H

/* Big Mac DAW — UI text rendering (SDL-based, embedded 5x7 bitmap font). */

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* draw a line of text; returns rendered width in px */
int wb_ui_draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale,
                    Uint8 cr, Uint8 cg, Uint8 cb);
int wb_ui_text_width(const char *text, int scale);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WB_UI_H */
