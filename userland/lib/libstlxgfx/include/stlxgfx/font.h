#ifndef STLXGFX_FONT_H
#define STLXGFX_FONT_H

#include <stlxgfx/surface.h>

#define STLXGFX_FONT_PATH "/initrd/res/fonts/UbuntuMono-Regular.ttf"

int stlxgfx_font_init(const char* font_path);
void stlxgfx_font_cleanup(void);

int stlxgfx_draw_text(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      const char* text, uint32_t font_size, uint32_t color);
void stlxgfx_text_size(const char* text, uint32_t font_size,
                       uint32_t* out_w, uint32_t* out_h);

#endif /* STLXGFX_FONT_H */
