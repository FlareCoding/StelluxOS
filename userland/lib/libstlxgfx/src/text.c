/* The text engine: font objects binding a face to one pixel size,
 * with metrics and per-face tables computed once at open, backed by
 * a process wide atlas of rasterized glyph coverage.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define STB_TRUETYPE_IMPLEMENTATION
#include <stlxgfx/internal/stb_truetype.h>
#undef STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic pop

#include <stlxgfx/internal/blend.h>
#include <stlxgfx/internal/text.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One shelf packed atlas page. A bumped generation is how eviction
 * invalidates the slots that pointed here. */
typedef struct {
    uint8_t* pixels;
    uint16_t gen;
    uint16_t shelf_y;
    uint16_t shelf_h;
    uint16_t cursor_x;
    uint64_t last_use;
} atlas_page;

static atlas_page g_pages[STLXGFX_ATLAS_MAX_PAGES];
static uint64_t g_use_tick = 0;
static stlxgfx_text_stats g_stats = { 0, 0, 0 };

static int32_t scale_round(float scale, int v) {
    float s = scale * (float)v;
    return (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
}

static uint8_t* read_whole_file(const char* path, long* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t* data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = size;
    return data;
}

/* Advances and kerning for printable ASCII are precomputed so the
 * measure and draw hot paths never call into the rasterizer */
static void cache_ascii_tables(stlxgfx_font* font) {
    for (int i = 0; i < STLXGFX_ASCII_COUNT; i++) {
        int cp = STLXGFX_ASCII_FIRST + i;
        int glyph = stbtt_FindGlyphIndex(&font->info, cp);
        font->ascii_glyph[i] = (uint16_t)glyph;

        int advance = 0;
        int lsb = 0;
        stbtt_GetGlyphHMetrics(&font->info, glyph, &advance, &lsb);
        font->ascii_advance[i] = (int16_t)scale_round(font->scale, advance);
    }

    font->has_kerning = 0;
    for (int a = 0; a < STLXGFX_ASCII_COUNT; a++) {
        for (int b = 0; b < STLXGFX_ASCII_COUNT; b++) {
            int kern = stbtt_GetGlyphKernAdvance(
                &font->info, font->ascii_glyph[a], font->ascii_glyph[b]);
            int32_t px = scale_round(font->scale, kern);
            if (px < -128) px = -128;
            if (px > 127) px = 127;

            font->ascii_kern[a][b] = (int8_t)px;
            if (px != 0) {
                font->has_kerning = 1;
            }
        }
    }
}

/* The face is monospaced when every printable glyph advances the
 * same, which is what lets a terminal skip measuring entirely */
static int32_t detect_mono_advance(const stlxgfx_font* font) {
    int16_t first = font->ascii_advance[0];
    for (int i = 1; i < STLXGFX_ASCII_COUNT; i++) {
        if (font->ascii_advance[i] != first) {
            return 0;
        }
    }

    return first;
}

stlxgfx_font* stlxgfx_font_open(const char* path, uint32_t px_size) {
    if (!path || px_size == 0) {
        return NULL;
    }

    long size = 0;
    uint8_t* data = read_whole_file(path, &size);
    if (!data) {
        return NULL;
    }

    stlxgfx_font* font = calloc(1, sizeof(*font));
    if (!font) {
        free(data);
        return NULL;
    }

    font->file_data = data;
    if (!stbtt_InitFont(&font->info, data,
                        stbtt_GetFontOffsetForIndex(data, 0))) {
        free(font);
        free(data);
        return NULL;
    }

    font->px_size = px_size;
    font->scale = stbtt_ScaleForPixelHeight(&font->info, (float)px_size);

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    font->metrics.ascent = scale_round(font->scale, ascent);
    font->metrics.descent = scale_round(font->scale, -descent);
    font->metrics.line_gap = scale_round(font->scale, line_gap);
    font->metrics.line_height = font->metrics.ascent
                              + font->metrics.descent
                              + font->metrics.line_gap;

    cache_ascii_tables(font);
    font->metrics.advance_mono = detect_mono_advance(font);

    return font;
}

void stlxgfx_font_close(stlxgfx_font* font) {
    if (!font) {
        return;
    }

    /* Atlas regions this font used are reclaimed by normal eviction */
    free(font->glyphs);
    free(font->file_data);
    free(font);
}

void stlxgfx_font_metrics_get(const stlxgfx_font* font,
                              stlxgfx_font_metrics* out) {
    *out = font->metrics;
}

/* Places a w by h region on a page shelf, or fails for this page */
static int page_place(atlas_page* p, uint16_t w, uint16_t h,
                      uint16_t* out_x, uint16_t* out_y) {
    if ((uint32_t)p->cursor_x + w <= STLXGFX_ATLAS_PAGE_W &&
        h <= p->shelf_h) {
        *out_x = p->cursor_x;
        *out_y = p->shelf_y;
        p->cursor_x = (uint16_t)(p->cursor_x + w);
        return 1;
    }

    uint32_t next_y = (uint32_t)p->shelf_y + p->shelf_h;
    if (next_y + h <= STLXGFX_ATLAS_PAGE_H &&
        w <= STLXGFX_ATLAS_PAGE_W) {
        p->shelf_y = (uint16_t)next_y;
        p->shelf_h = h;
        p->cursor_x = w;
        *out_x = 0;
        *out_y = (uint16_t)next_y;
        return 1;
    }

    return 0;
}

/* Reserves atlas space: an existing page with room, then a fresh
 * page, then the least recently used page evicted and reused */
static int atlas_reserve(uint16_t w, uint16_t h, uint16_t* out_page,
                         uint16_t* out_x, uint16_t* out_y) {
    if (w > STLXGFX_ATLAS_PAGE_W || h > STLXGFX_ATLAS_PAGE_H) {
        return -1;
    }

    for (uint16_t i = 0; i < STLXGFX_ATLAS_MAX_PAGES; i++) {
        atlas_page* p = &g_pages[i];
        if (p->pixels && page_place(p, w, h, out_x, out_y)) {
            *out_page = i;
            p->last_use = ++g_use_tick;
            return 0;
        }
    }

    for (uint16_t i = 0; i < STLXGFX_ATLAS_MAX_PAGES; i++) {
        atlas_page* p = &g_pages[i];
        if (p->pixels) {
            continue;
        }

        p->pixels = calloc(1, STLXGFX_ATLAS_PAGE_W * STLXGFX_ATLAS_PAGE_H);
        if (!p->pixels) {
            break;
        }

        page_place(p, w, h, out_x, out_y);
        *out_page = i;
        p->last_use = ++g_use_tick;
        return 0;
    }

    uint16_t victim = 0;
    for (uint16_t i = 1; i < STLXGFX_ATLAS_MAX_PAGES; i++) {
        if (g_pages[i].pixels &&
            g_pages[i].last_use < g_pages[victim].last_use) {
            victim = i;
        }
    }

    atlas_page* p = &g_pages[victim];
    if (!p->pixels) {
        return -1;
    }

    /* The bump invalidates every slot that pointed at this page */
    p->gen++;
    p->shelf_y = 0;
    p->shelf_h = 0;
    p->cursor_x = 0;
    g_stats.page_evictions++;

    page_place(p, w, h, out_x, out_y);
    *out_page = victim;
    p->last_use = ++g_use_tick;
    return 0;
}

static stlxgfx_glyph_slot* map_probe(stlxgfx_glyph_slot* table,
                                     uint32_t cap, uint32_t codepoint) {
    uint32_t i = (codepoint * 2654435761u) & (cap - 1);
    while (table[i].codepoint != 0 && table[i].codepoint != codepoint) {
        i = (i + 1) & (cap - 1);
    }

    return &table[i];
}

static int map_grow(stlxgfx_font* font) {
    uint32_t new_cap = font->glyph_cap ? font->glyph_cap * 2 : 64;
    stlxgfx_glyph_slot* table = calloc(new_cap, sizeof(*table));
    if (!table) {
        return -1;
    }

    for (uint32_t i = 0; i < font->glyph_cap; i++) {
        if (font->glyphs[i].codepoint != 0) {
            *map_probe(table, new_cap, font->glyphs[i].codepoint) =
                font->glyphs[i];
        }
    }

    free(font->glyphs);
    font->glyphs = table;
    font->glyph_cap = new_cap;
    return 0;
}

const stlxgfx_glyph_slot* stlxgfx_glyph_get(stlxgfx_font* font,
                                            uint32_t codepoint) {
    if (codepoint == 0) {
        return NULL;
    }

    g_stats.lookups++;

    if (font->glyph_count * 10 >= font->glyph_cap * 7 &&
        map_grow(font) != 0) {
        return NULL;
    }

    stlxgfx_glyph_slot* slot =
        map_probe(font->glyphs, font->glyph_cap, codepoint);
    if (slot->codepoint == codepoint) {
        atlas_page* p = &g_pages[slot->page];
        if (slot->w == 0 || slot->gen == p->gen) {
            p->last_use = ++g_use_tick;
            return slot;
        }
    }

    int glyph;
    int16_t advance;
    if (codepoint >= STLXGFX_ASCII_FIRST &&
        codepoint < STLXGFX_ASCII_FIRST + STLXGFX_ASCII_COUNT) {
        glyph = font->ascii_glyph[codepoint - STLXGFX_ASCII_FIRST];
        advance = font->ascii_advance[codepoint - STLXGFX_ASCII_FIRST];
    } else {
        glyph = stbtt_FindGlyphIndex(&font->info, (int)codepoint);
        int adv = 0;
        int lsb = 0;
        stbtt_GetGlyphHMetrics(&font->info, glyph, &adv, &lsb);
        advance = (int16_t)scale_round(font->scale, adv);
    }

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&font->info, glyph, font->scale, font->scale,
                            &x0, &y0, &x1, &y1);
    uint16_t w = (uint16_t)(x1 - x0);
    uint16_t h = (uint16_t)(y1 - y0);

    if (slot->codepoint == 0) {
        font->glyph_count++;
    }
    slot->codepoint = codepoint;
    slot->glyph_index = (uint16_t)glyph;
    slot->bearing_x = (int16_t)x0;
    slot->bearing_y = (int16_t)y0;
    slot->advance = advance;

    /* Whitespace caches as a slot without atlas space */
    if (w == 0 || h == 0) {
        slot->page = 0;
        slot->gen = 0;
        slot->x = 0;
        slot->y = 0;
        slot->w = 0;
        slot->h = 0;
        return slot;
    }

    uint16_t page, gx, gy;
    if (atlas_reserve(w, h, &page, &gx, &gy) != 0) {
        slot->codepoint = 0;
        font->glyph_count--;
        return NULL;
    }

    stbtt_MakeGlyphBitmap(&font->info,
                          g_pages[page].pixels
                              + (uint32_t)gy * STLXGFX_ATLAS_PAGE_W + gx,
                          w, h, STLXGFX_ATLAS_PAGE_W,
                          font->scale, font->scale, glyph);
    g_stats.rasterizations++;

    slot->page = page;
    slot->gen = g_pages[page].gen;
    slot->x = gx;
    slot->y = gy;
    slot->w = w;
    slot->h = h;
    return slot;
}

const uint8_t* stlxgfx_atlas_page_pixels(uint16_t page) {
    if (page >= STLXGFX_ATLAS_MAX_PAGES) {
        return NULL;
    }

    return g_pages[page].pixels;
}

void stlxgfx_text_stats_get(stlxgfx_text_stats* out) {
    *out = g_stats;
}

/* Decodes the next UTF-8 codepoint, advancing the byte index. A
 * malformed byte is consumed alone and decodes to 0, which renders
 * and measures as nothing. */
static uint32_t utf8_next(const char* s, size_t len, size_t* i) {
    uint8_t b0 = (uint8_t)s[(*i)++];
    if (b0 < 0x80) {
        return b0;
    }

    uint32_t cp;
    size_t extra;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1Fu;
        extra = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0Fu;
        extra = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07u;
        extra = 3;
    } else {
        return 0;
    }

    if (*i + extra > len) {
        *i = len;
        return 0;
    }

    for (size_t k = 0; k < extra; k++) {
        uint8_t c = (uint8_t)s[*i];
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (c & 0x3Fu);
        (*i)++;
    }

    return cp;
}

/* Pair kerning in pixels, table hit for ASCII pairs and a rasterizer
 * lookup otherwise. Faces without kerning skip all of it. */
static int32_t kern_px(const stlxgfx_font* font,
                       uint32_t prev_cp, uint16_t prev_glyph,
                       uint32_t cp, uint16_t cur_glyph) {
    if (!font->has_kerning || prev_cp == 0) {
        return 0;
    }

    if (prev_cp >= STLXGFX_ASCII_FIRST &&
        prev_cp < STLXGFX_ASCII_FIRST + STLXGFX_ASCII_COUNT &&
        cp >= STLXGFX_ASCII_FIRST &&
        cp < STLXGFX_ASCII_FIRST + STLXGFX_ASCII_COUNT) {
        return font->ascii_kern[prev_cp - STLXGFX_ASCII_FIRST]
                               [cp - STLXGFX_ASCII_FIRST];
    }

    int kern = stbtt_GetGlyphKernAdvance(&font->info, prev_glyph, cur_glyph);
    return scale_round(font->scale, kern);
}

int32_t stlxgfx_text_width(const stlxgfx_font* font,
                           const char* utf8, size_t len) {
    if (!font || !utf8) {
        return 0;
    }

    int32_t width = 0;
    uint32_t prev_cp = 0;
    uint16_t prev_glyph = 0;
    size_t i = 0;

    while (i < len) {
        uint32_t cp = utf8_next(utf8, len, &i);
        if (cp == 0) {
            continue;
        }

        const stlxgfx_glyph_slot* slot =
            stlxgfx_glyph_get((stlxgfx_font*)font, cp);
        if (!slot) {
            continue;
        }

        width += slot->advance
               + kern_px(font, prev_cp, prev_glyph, cp, slot->glyph_index);
        prev_cp = cp;
        prev_glyph = slot->glyph_index;
    }

    return width;
}

void stlxgfx_draw_text(stlxgfx_surface_t* s, const stlxgfx_font* font,
                       int32_t x, int32_t baseline_y,
                       const char* utf8, size_t len, uint32_t color) {
    if (!s || !font || !utf8) {
        return;
    }

    /* Channel layout and color are invariant across the run */
    uint32_t bytes_pp = s->bpp / 8;
    uint8_t rb = s->red_shift   / 8;
    uint8_t gb = s->green_shift / 8;
    uint8_t bb = s->blue_shift  / 8;
    uint8_t ab = stlxgfx_alpha_byte_index(s);
    int has_alpha = bytes_pp == 4;
    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >>  8) & 0xFF;
    uint32_t src_b =  color        & 0xFF;
    uint32_t color_a = color >> 24;

    int32_t pen_x = x;
    uint32_t prev_cp = 0;
    uint16_t prev_glyph = 0;
    size_t i = 0;

    while (i < len) {
        uint32_t cp = utf8_next(utf8, len, &i);
        if (cp == 0) {
            continue;
        }

        stlxgfx_glyph_slot slot_copy;
        {
            const stlxgfx_glyph_slot* slot =
                stlxgfx_glyph_get((stlxgfx_font*)font, cp);
            if (!slot) {
                continue;
            }
            slot_copy = *slot;
        }

        pen_x += kern_px(font, prev_cp, prev_glyph, cp,
                         slot_copy.glyph_index);
        prev_cp = cp;
        prev_glyph = slot_copy.glyph_index;

        if (slot_copy.w == 0) {
            pen_x += slot_copy.advance;
            continue;
        }

        const uint8_t* page = g_pages[slot_copy.page].pixels;
        int32_t gx = pen_x + slot_copy.bearing_x;
        int32_t gy = baseline_y + slot_copy.bearing_y;

        for (uint16_t r = 0; r < slot_copy.h; r++) {
            int32_t sy = gy + r;
            if (sy < 0 || sy >= (int32_t)s->height) {
                continue;
            }

            const uint8_t* cov = page
                + (uint32_t)(slot_copy.y + r) * STLXGFX_ATLAS_PAGE_W
                + slot_copy.x;
            uint8_t* row = s->pixels + (uint32_t)sy * s->pitch;

            for (uint16_t c = 0; c < slot_copy.w; c++) {
                int32_t sx = gx + c;
                if (sx < 0 || sx >= (int32_t)s->width) {
                    continue;
                }

                uint32_t a = cov[c];
                if (a == 0) {
                    continue;
                }
                a = color_a == 255 ? a : stlxgfx_div255(a * color_a);

                uint8_t* pixel = row + (uint32_t)sx * bytes_pp;
                if (a == 255) {
                    pixel[rb] = (uint8_t)src_r;
                    pixel[gb] = (uint8_t)src_g;
                    pixel[bb] = (uint8_t)src_b;
                    if (has_alpha) {
                        pixel[ab] = 0xFF;
                    }
                    continue;
                }

                uint32_t inv = 255 - a;
                pixel[rb] = stlxgfx_blend_channel(pixel[rb], src_r * a, inv);
                pixel[gb] = stlxgfx_blend_channel(pixel[gb], src_g * a, inv);
                pixel[bb] = stlxgfx_blend_channel(pixel[bb], src_b * a, inv);
                if (has_alpha) {
                    pixel[ab] = 0xFF;
                }
            }
        }

        pen_x += slot_copy.advance;
    }
}
