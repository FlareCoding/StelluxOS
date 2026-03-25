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
