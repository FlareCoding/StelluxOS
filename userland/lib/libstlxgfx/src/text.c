/* The text engine: font objects binding a face to one pixel size,
 * with metrics and per-face tables computed once at open.
 */
#include <stlxgfx/internal/text.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    free(font->file_data);
    free(font);
}

void stlxgfx_font_metrics_get(const stlxgfx_font* font,
                              stlxgfx_font_metrics* out) {
    *out = font->metrics;
}
