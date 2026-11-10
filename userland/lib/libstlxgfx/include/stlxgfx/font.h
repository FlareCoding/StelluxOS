#ifndef STLXGFX_FONT_H
#define STLXGFX_FONT_H

#include <stlxgfx/surface.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A typeface bound to one size. Metrics, kerning, and rendered
 * glyph coverage are cached inside the object, so opening a font is
 * the expensive step and everything after it is cheap.
 */
typedef struct stlxgfx_font stlxgfx_font;

/**
 * Vertical metrics and the fixed advance, all in pixels. Ascent is
 * the extent above the baseline and descent the extent below it,
 * both positive. One line of text occupies ascent + descent pixels
 * and consecutive baselines sit line_height apart.
 */
typedef struct {
    int32_t ascent;
    int32_t descent;
    int32_t line_gap;
    int32_t line_height;   /* ascent + descent + line_gap */
    int32_t advance_mono;  /* fixed advance, 0 for proportional faces */
} stlxgfx_font_metrics;

/**
 * @brief Opens a TrueType face at one size.
 * @param path Font file path, for example `STLXTHEME_MONO_FONT_PATH`.
 * @param em_px Height of the face's em square in pixels, the size
 * CSS and Qt give a font.
 * @return Font object, or NULL when `path` is NULL, `em_px` is not
 * above 0, the file is missing or invalid, or memory runs out.
 */
stlxgfx_font* stlxgfx_font_open(const char* path, float em_px);

/**
 * @brief Releases a font and every cached glyph it holds.
 * @param font Font to close, NULL is ignored.
 */
void stlxgfx_font_close(stlxgfx_font* font);

/**
 * @brief Reads the font's vertical metrics and fixed advance.
 * @param font Font to query.
 * @param out Filled with the metrics.
 */
void stlxgfx_font_metrics_get(const stlxgfx_font* font,
                              stlxgfx_font_metrics* out);

/**
 * @brief Measures the advance width of a UTF-8 string, kerning
 * applied. Malformed bytes are skipped.
 * @param font Font to measure with.
 * @param utf8 Text, not required to be NUL terminated.
 * @param len Byte length of the text.
 * @return Width in pixels from pen start to pen end.
 */
int32_t stlxgfx_text_width(const stlxgfx_font* font,
                           const char* utf8, size_t len);

/**
 * @brief Draws a UTF-8 string with the pen on the baseline, kerning
 * applied. Mixed sizes on one line align by giving each draw the
 * same baseline. Malformed bytes are skipped.
 * @param s Target surface.
 * @param font Font to draw with.
 * @param x Pen start in pixels.
 * @param baseline_y Baseline in pixels, not a bounding box corner.
 * @param utf8 Text, not required to be NUL terminated.
 * @param len Byte length of the text.
 * @param color ARGB text color, glyph coverage blends onto s.
 */
void stlxgfx_draw_text(stlxgfx_surface_t* s, const stlxgfx_font* font,
                       int32_t x, int32_t baseline_y,
                       const char* utf8, size_t len, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* STLXGFX_FONT_H */
