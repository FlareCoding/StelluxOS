#include <stlxgfx/surface.h>
#include <stdlib.h>
#include <string.h>

static inline void write_pixel(uint8_t* pixel, const stlxgfx_surface_t* s, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b =  color        & 0xFF;

    uint32_t bytes_pp = s->bpp / 8;
    pixel[s->red_shift   / 8] = r;
    pixel[s->green_shift / 8] = g;
    pixel[s->blue_shift  / 8] = b;
    if (bytes_pp == 4) {
        uint8_t a = (color >> 24) & 0xFF;
        uint8_t alpha_byte = 3;
        if (s->red_shift != 0 && s->green_shift != 0 && s->blue_shift != 0) {
            alpha_byte = 0;
        }
        pixel[alpha_byte] = a;
    }
}

stlxgfx_surface_t* stlxgfx_create_surface(uint32_t width, uint32_t height,
                                           uint32_t bpp, uint8_t red_shift,
                                           uint8_t green_shift, uint8_t blue_shift) {
    if (width == 0 || height == 0 || (bpp != 24 && bpp != 32)) {
        return NULL;
    }

    stlxgfx_surface_t* s = malloc(sizeof(stlxgfx_surface_t));
    if (!s) {
        return NULL;
    }

    uint32_t bytes_pp = bpp / 8;
    s->width       = width;
    s->height      = height;
    s->pitch       = width * bytes_pp;
    s->bpp         = bpp;
    s->red_shift   = red_shift;
    s->green_shift = green_shift;
    s->blue_shift  = blue_shift;
    s->owned       = 1;

    s->pixels = malloc((size_t)s->pitch * height);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    memset(s->pixels, 0, (size_t)s->pitch * height);

    return s;
}

void stlxgfx_destroy_surface(stlxgfx_surface_t* surface) {
    if (!surface) {
        return;
    }
    if (surface->owned && surface->pixels) {
        free(surface->pixels);
    }
    free(surface);
}

stlxgfx_surface_t* stlxgfx_surface_from_buffer(uint8_t* buffer,
                                                uint32_t width, uint32_t height,
                                                uint32_t pitch, uint32_t bpp,
                                                uint8_t red_shift,
                                                uint8_t green_shift,
                                                uint8_t blue_shift) {
    if (!buffer || width == 0 || height == 0) {
        return NULL;
    }

    stlxgfx_surface_t* s = malloc(sizeof(stlxgfx_surface_t));
    if (!s) {
        return NULL;
    }

    s->width       = width;
    s->height      = height;
    s->pitch       = pitch;
    s->bpp         = bpp;
    s->red_shift   = red_shift;
    s->green_shift = green_shift;
    s->blue_shift  = blue_shift;
    s->pixels      = buffer;
    s->owned       = 0;

    return s;
}

int stlxgfx_clear(stlxgfx_surface_t* s, uint32_t color) {
    if (!s || !s->pixels) {
        return -1;
    }
    return stlxgfx_fill_rect(s, 0, 0, s->width, s->height, color);
}

int stlxgfx_fill_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t color) {
    if (!s || !s->pixels) {
        return -1;
    }

    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = (int32_t)(x + w);
    int32_t y1 = (int32_t)(y + h);
    if (x1 > (int32_t)s->width)  x1 = (int32_t)s->width;
    if (y1 > (int32_t)s->height) y1 = (int32_t)s->height;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    uint32_t bytes_pp = s->bpp / 8;
    for (int32_t row = y0; row < y1; row++) {
        uint8_t* row_ptr = s->pixels + (uint32_t)row * s->pitch + (uint32_t)x0 * bytes_pp;
        for (int32_t col = x0; col < x1; col++) {
            write_pixel(row_ptr, s, color);
            row_ptr += bytes_pp;
        }
    }
    return 0;
}

int stlxgfx_draw_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t color) {
    if (!s || w == 0 || h == 0) {
        return -1;
    }
    stlxgfx_fill_rect(s, x, y, w, 1, color);
    if (h > 1) {
        stlxgfx_fill_rect(s, x, y + (int32_t)h - 1, w, 1, color);
    }
    if (h > 2) {
        stlxgfx_fill_rect(s, x, y + 1, 1, h - 2, color);
        if (w > 1) {
            stlxgfx_fill_rect(s, x + (int32_t)w - 1, y + 1, 1, h - 2, color);
        }
    }
    return 0;
}

int stlxgfx_blit(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                 const stlxgfx_surface_t* src, int32_t sx, int32_t sy,
                 uint32_t w, uint32_t h) {
    if (!dst || !dst->pixels || !src || !src->pixels) {
        return -1;
    }

    int32_t sw = (int32_t)w;
    int32_t sh = (int32_t)h;

    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }

    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    if ((uint32_t)sx + (uint32_t)sw > src->width)  sw = (int32_t)(src->width  - (uint32_t)sx);
    if ((uint32_t)sy + (uint32_t)sh > src->height) sh = (int32_t)(src->height - (uint32_t)sy);
    if ((uint32_t)dx + (uint32_t)sw > dst->width)  sw = (int32_t)(dst->width  - (uint32_t)dx);
    if ((uint32_t)dy + (uint32_t)sh > dst->height) sh = (int32_t)(dst->height - (uint32_t)dy);

    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    uint32_t bytes_pp = dst->bpp / 8;
    size_t row_bytes = (size_t)(uint32_t)sw * bytes_pp;

    for (int32_t row = 0; row < sh; row++) {
        uint8_t* src_row = src->pixels + ((uint32_t)sy + (uint32_t)row) * src->pitch + (uint32_t)sx * bytes_pp;
        uint8_t* dst_row = dst->pixels + ((uint32_t)dy + (uint32_t)row) * dst->pitch + (uint32_t)dx * bytes_pp;
        memcpy(dst_row, src_row, row_bytes);
    }
    return 0;
}
