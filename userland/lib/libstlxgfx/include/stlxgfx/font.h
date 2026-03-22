#ifndef STLXGFX_FONT_H
#define STLXGFX_FONT_H

#include <stlxgfx/surface.h>

#define STLXGFX_FONT_WIDTH  8
#define STLXGFX_FONT_HEIGHT 16

int stlxgfx_draw_char(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      char c, uint32_t fg, uint32_t bg);
int stlxgfx_draw_text(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      const char* text, uint32_t fg, uint32_t bg);
void stlxgfx_text_size(const char* text, uint32_t* out_w, uint32_t* out_h);

#endif /* STLXGFX_FONT_H */
