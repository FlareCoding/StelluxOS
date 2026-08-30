#ifndef STLXGFX_INTERNAL_TEXT_H
#define STLXGFX_INTERNAL_TEXT_H

#include <stlxgfx/font.h>
#include <stlxgfx/internal/stb_truetype.h>

/* Printable ASCII is precached at open, everything else renders
 * through the glyph cache on demand */
#define STLXGFX_ASCII_FIRST 32
#define STLXGFX_ASCII_COUNT 95

/* A typeface bound to one pixel size. The rasterizer state and the
 * per-face tables live here, the glyph atlas attaches later. */
struct stlxgfx_font {
    uint8_t* file_data;
    stbtt_fontinfo info;
    uint32_t px_size;
    float scale;
    stlxgfx_font_metrics metrics;

    /* Precached printable ASCII, the overwhelmingly common range */
    uint16_t ascii_glyph[STLXGFX_ASCII_COUNT];
    int16_t ascii_advance[STLXGFX_ASCII_COUNT];
    int8_t ascii_kern[STLXGFX_ASCII_COUNT][STLXGFX_ASCII_COUNT];
    int has_kerning;
};

#endif /* STLXGFX_INTERNAL_TEXT_H */
