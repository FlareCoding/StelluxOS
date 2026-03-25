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

int stlxgfx_fill_circle(stlxgfx_surface_t* s, int32_t cx, int32_t cy,
                         uint32_t radius, uint32_t color) {
    if (!s || !s->pixels || radius == 0) {
        return -1;
    }
    int32_t r = (int32_t)radius;
    int32_t x = 0;
    int32_t y = r;
    int32_t d = 1 - r;

    stlxgfx_fill_rect(s, cx - r, cy, (uint32_t)(2 * r + 1), 1, color);

    while (x < y) {
        x++;
        if (d < 0) {
            d += 2 * x + 1;
        } else {
            y--;
            d += 2 * (x - y) + 1;
        }
        stlxgfx_fill_rect(s, cx - x, cy + y, (uint32_t)(2 * x + 1), 1, color);
        stlxgfx_fill_rect(s, cx - x, cy - y, (uint32_t)(2 * x + 1), 1, color);
        stlxgfx_fill_rect(s, cx - y, cy + x, (uint32_t)(2 * y + 1), 1, color);
        stlxgfx_fill_rect(s, cx - y, cy - x, (uint32_t)(2 * y + 1), 1, color);
    }
    return 0;
}

int stlxgfx_fill_rounded_rect(stlxgfx_surface_t* s, int32_t x, int32_t y,
                               uint32_t w, uint32_t h, uint32_t radius,
                               uint32_t color) {
    if (!s || !s->pixels || w == 0 || h == 0) {
        return -1;
    }
    uint32_t max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius == 0) {
        return stlxgfx_fill_rect(s, x, y, w, h, color);
    }

    int32_t r = (int32_t)radius;

    stlxgfx_fill_rect(s, x + r, y, w - 2 * radius, h, color);
    stlxgfx_fill_rect(s, x, y + r, (uint32_t)r, h - 2 * radius, color);
    stlxgfx_fill_rect(s, x + (int32_t)w - r, y + r, (uint32_t)r, h - 2 * radius, color);

    int32_t cx_tl = x + r;
    int32_t cy_tl = y + r;
    int32_t cx_tr = x + (int32_t)w - r - 1;
    int32_t cy_tr = y + r;
    int32_t cx_bl = x + r;
    int32_t cy_bl = y + (int32_t)h - r - 1;
    int32_t cx_br = x + (int32_t)w - r - 1;
    int32_t cy_br = y + (int32_t)h - r - 1;

    int32_t px = 0;
    int32_t py = r;
    int32_t d = 1 - r;

    while (px <= py) {
        stlxgfx_fill_rect(s, cx_tl - px, cy_tl - py, (uint32_t)(px + 1), 1, color);
        stlxgfx_fill_rect(s, cx_tr,      cy_tr - py, (uint32_t)(px + 1), 1, color);
        stlxgfx_fill_rect(s, cx_bl - px, cy_bl + py, (uint32_t)(px + 1), 1, color);
        stlxgfx_fill_rect(s, cx_br,      cy_br + py, (uint32_t)(px + 1), 1, color);

        stlxgfx_fill_rect(s, cx_tl - py, cy_tl - px, (uint32_t)(py + 1), 1, color);
        stlxgfx_fill_rect(s, cx_tr,      cy_tr - px, (uint32_t)(py + 1), 1, color);
        stlxgfx_fill_rect(s, cx_bl - py, cy_bl + px, (uint32_t)(py + 1), 1, color);
        stlxgfx_fill_rect(s, cx_br,      cy_br + px, (uint32_t)(py + 1), 1, color);

        px++;
        if (d < 0) {
            d += 2 * px + 1;
        } else {
            py--;
            d += 2 * (px - py) + 1;
        }
    }
    return 0;
}
