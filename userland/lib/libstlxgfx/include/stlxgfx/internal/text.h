#ifndef STLXGFX_INTERNAL_TEXT_H
#define STLXGFX_INTERNAL_TEXT_H

#include <stlxgfx/font.h>
#include <stlxgfx/internal/stb_truetype.h>

/* Printable ASCII is precached at open, everything else renders
 * through the glyph cache on demand */
#define STLXGFX_ASCII_FIRST 32
#define STLXGFX_ASCII_COUNT 95

/* The atlas arena is process global: pages of packed A8 coverage
 * shared by every open font, bounded by the page count. Evicting one
 * page bumps its generation, which lazily invalidates exactly the
 * slots that lived on it and nothing else. */
#define STLXGFX_ATLAS_PAGE_W 512
#define STLXGFX_ATLAS_PAGE_H 512
#define STLXGFX_ATLAS_MAX_PAGES 4

/* One cached glyph: its atlas region, placement relative to the pen
 * (bearing_y is the offset from the baseline to the bitmap top,
 * negative above), and its advance. Valid while gen matches the
 * owning page. */
typedef struct {
    uint32_t codepoint;    /* 0 marks an empty map slot */
    uint16_t page;
    uint16_t gen;
    uint16_t x, y, w, h;
    int16_t bearing_x;
    int16_t bearing_y;
    int16_t advance;
} stlxgfx_glyph_slot;

/* Cache observability for tests: a full flush cannot appear here
 * because no code path exists to perform one */
typedef struct {
    uint64_t lookups;
    uint64_t rasterizations;
    uint64_t page_evictions;
} stlxgfx_text_stats;

/* A typeface bound to one pixel size. The rasterizer state, the
 * per-face tables, and the codepoint map into the atlas live here. */
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

    /* Open addressed codepoint map, linear probing, grown at 70% */
    stlxgfx_glyph_slot* glyphs;
    uint32_t glyph_cap;
    uint32_t glyph_count;
};

/* Returns the cached slot for a codepoint, rasterizing on miss and
 * evicting the least recently used page when the arena is full.
 * NULL when the glyph cannot be cached. */
const stlxgfx_glyph_slot* stlxgfx_glyph_get(stlxgfx_font* font,
                                            uint32_t codepoint);

/* Page pixels for blitting a slot's region, NULL for a bad index. */
const uint8_t* stlxgfx_atlas_page_pixels(uint16_t page);

void stlxgfx_text_stats_get(stlxgfx_text_stats* out);

#endif /* STLXGFX_INTERNAL_TEXT_H */
