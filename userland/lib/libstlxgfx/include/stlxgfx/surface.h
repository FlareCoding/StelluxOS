#ifndef STLXGFX_SURFACE_H
#define STLXGFX_SURFACE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t* pixels;
    int      owned;
} stlxgfx_surface_t;

static inline uint8_t stlxgfx_alpha_byte_index(const stlxgfx_surface_t* s) {
    if (s->red_shift != 0 && s->green_shift != 0 && s->blue_shift != 0)
        return 0;
    return 3;
}

stlxgfx_surface_t* stlxgfx_create_surface(uint32_t width, uint32_t height,
                                           uint32_t bpp, uint8_t red_shift,
                                           uint8_t green_shift, uint8_t blue_shift);
void stlxgfx_destroy_surface(stlxgfx_surface_t* surface);

stlxgfx_surface_t* stlxgfx_surface_from_buffer(uint8_t* buffer,
                                                uint32_t width, uint32_t height,
                                                uint32_t pitch, uint32_t bpp,
                                                uint8_t red_shift,
                                                uint8_t green_shift,
                                                uint8_t blue_shift);

int stlxgfx_clear(stlxgfx_surface_t* s, uint32_t color);
int stlxgfx_fill_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t color);

/* Source-over rect fill: blends the color by its alpha instead of
 * writing it raw, for translucent UI drawing. */
int stlxgfx_fill_rect_blend(stlxgfx_surface_t* s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color);

/* Source-over single pixel with extra coverage (0-255) modulating the
 * color's alpha, the building block for anti-aliased edges. */
void stlxgfx_blend_coverage(stlxgfx_surface_t* s, int32_t x, int32_t y,
                            uint32_t color, uint8_t coverage);

/* Alpha blit clipped to a rounded rect: source pixels blend by their
 * alpha times anti-aliased corner coverage, so square images composite
 * as rounded tiles. Corner arcs anchor on the logical (dx, dy, w, h)
 * rect even when partially off-surface. */
int stlxgfx_blit_rounded_alpha(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                               const stlxgfx_surface_t* src,
                               int32_t sx, int32_t sy,
                               uint32_t w, uint32_t h, uint32_t radius);

/* Bilinear blit of the source rect (sx, sy, sw, sh) onto the dest
 * rect (dx, dy, dw, dh), resampling when the sizes differ. */
int stlxgfx_blit_scaled(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                        uint32_t dw, uint32_t dh,
                        const stlxgfx_surface_t* src, int32_t sx, int32_t sy,
                        uint32_t sw, uint32_t sh);

/* Anti-aliased corner arc blit, the image-sampling twin of
 * stlxgfx_ctx_fill_arc_corner: fills the r_outer-by-r_outer pixel box
 * at rect corner (x, y), growing inward along (dir_x, dir_y), with
 * pixels sampled from src at the same coordinates. Fills inside the
 * arc, a nonzero r_inner leaves the concentric inner disc empty, and
 * invert fills outside the arc instead, which restores a background
 * around a rounded corner. src must cover the touched coordinates. */
void stlxgfx_blit_arc_corner(stlxgfx_surface_t* dst, int32_t x, int32_t y,
                             uint32_t r_outer, uint32_t r_inner,
                             int dir_x, int dir_y, int invert,
                             const stlxgfx_surface_t* src);
int stlxgfx_draw_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t color);
int stlxgfx_blit(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                 const stlxgfx_surface_t* src, int32_t sx, int32_t sy,
                 uint32_t w, uint32_t h);

int stlxgfx_fill_circle(stlxgfx_surface_t* s, int32_t cx, int32_t cy,
                         uint32_t radius, uint32_t color);
int stlxgfx_fill_rounded_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                               uint32_t w, uint32_t h, uint32_t radius,
                               uint32_t color);

int stlxgfx_blit_alpha(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                        const stlxgfx_surface_t* src, int32_t sx, int32_t sy,
                        uint32_t w, uint32_t h);

int stlxgfx_draw_line(stlxgfx_surface_t* s, int32_t x0, int32_t y0,
                       int32_t x1, int32_t y1, uint32_t color);

#endif /* STLXGFX_SURFACE_H */
