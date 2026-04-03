#ifndef STLXGFX_FB_H
#define STLXGFX_FB_H

#include <stlxgfx/surface.h>

typedef struct {
    int      fd;
    uint8_t* buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint64_t size;
} stlxgfx_fb_t;

int  stlxgfx_fb_open(stlxgfx_fb_t* fb);
void stlxgfx_fb_close(stlxgfx_fb_t* fb);

stlxgfx_surface_t* stlxgfx_fb_surface(stlxgfx_fb_t* fb);
stlxgfx_surface_t* stlxgfx_fb_create_surface(const stlxgfx_fb_t* fb,
                                               uint32_t width, uint32_t height);

void stlxgfx_fb_present(stlxgfx_fb_t* fb, const stlxgfx_surface_t* surface);

void stlxgfx_fb_present_region(stlxgfx_fb_t* fb,
                                const stlxgfx_surface_t* surface,
                                int32_t x, int32_t y,
                                uint32_t w, uint32_t h);

#endif /* STLXGFX_FB_H */
