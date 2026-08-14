#include <stlxgfx/surface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t read_pixel(const uint8_t* pixel, const stlxgfx_surface_t* s) {
    uint8_t r = pixel[s->red_shift   / 8];
    uint8_t g = pixel[s->green_shift / 8];
    uint8_t b = pixel[s->blue_shift  / 8];
    uint8_t a = 0xFF;
    if (s->bpp == 32)
        a = pixel[stlxgfx_alpha_byte_index(s)];
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void write_pixel(uint8_t* pixel, const stlxgfx_surface_t* s, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b =  color        & 0xFF;

    pixel[s->red_shift   / 8] = r;
    pixel[s->green_shift / 8] = g;
    pixel[s->blue_shift  / 8] = b;
    if (s->bpp / 8 == 4)
        pixel[stlxgfx_alpha_byte_index(s)] = (color >> 24) & 0xFF;
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
    if (x1 > (int32_t)s->width) {
        x1 = (int32_t)s->width;
    }
    if (y1 > (int32_t)s->height) {
        y1 = (int32_t)s->height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    uint32_t bytes_pp = s->bpp / 8;

    /* Fast path for 32bpp: single uint32_t write per pixel */
    if (bytes_pp == 4) {
        uint32_t native_pixel;
        uint8_t* np = (uint8_t*)&native_pixel;
        np[s->red_shift   / 8] = (color >> 16) & 0xFF;
        np[s->green_shift / 8] = (color >>  8) & 0xFF;
        np[s->blue_shift  / 8] =  color        & 0xFF;
        np[stlxgfx_alpha_byte_index(s)] = (color >> 24) & 0xFF;

        uint32_t count = (uint32_t)(x1 - x0);
        for (int32_t row = y0; row < y1; row++) {
            uint32_t* row_ptr = (uint32_t*)(s->pixels + (uint32_t)row * s->pitch)
                              + (uint32_t)x0;
            for (uint32_t i = 0; i < count; i++) {
                row_ptr[i] = native_pixel;
            }
        }
        return 0;
    }

    /* Fallback for 24bpp */
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

    if ((uint32_t)sx + (uint32_t)sw > src->width) {
        sw = (int32_t)(src->width  - (uint32_t)sx);
    }
    if ((uint32_t)sy + (uint32_t)sh > src->height) {
        sh = (int32_t)(src->height - (uint32_t)sy);
    }
    if ((uint32_t)dx + (uint32_t)sw > dst->width) {
        sw = (int32_t)(dst->width  - (uint32_t)dx);
    }
    if ((uint32_t)dy + (uint32_t)sh > dst->height) {
        sh = (int32_t)(dst->height - (uint32_t)dy);
    }

    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    uint32_t dst_bpp = dst->bpp / 8;
    uint32_t src_bpp = src->bpp / 8;
    uint32_t copy_bpp = dst_bpp < src_bpp ? dst_bpp : src_bpp;
    size_t row_bytes = (size_t)(uint32_t)sw * copy_bpp;

    for (int32_t row = 0; row < sh; row++) {
        uint8_t* src_row = src->pixels + ((uint32_t)sy + (uint32_t)row) * src->pitch + (uint32_t)sx * src_bpp;
        uint8_t* dst_row = dst->pixels + ((uint32_t)dy + (uint32_t)row) * dst->pitch + (uint32_t)dx * dst_bpp;
        if (src_bpp == dst_bpp) {
            memcpy(dst_row, src_row, row_bytes);
        } else {
            for (int32_t col = 0; col < sw; col++) {
                uint32_t c = read_pixel(src_row, src);
                write_pixel(dst_row, dst, c);
                src_row += src_bpp;
                dst_row += dst_bpp;
            }
        }
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
    if (radius > max_r) {
        radius = max_r;
    }
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

static inline void blend_pixel(uint8_t* dst_px, const stlxgfx_surface_t* dst,
                                uint32_t src_color) {
    uint8_t sa = (src_color >> 24) & 0xFF;
    if (sa == 0) {
        return;
    }
    if (sa == 255) {
        write_pixel(dst_px, dst, src_color);
        return;
    }
    uint8_t sr = (src_color >> 16) & 0xFF;
    uint8_t sg = (src_color >>  8) & 0xFF;
    uint8_t sb =  src_color        & 0xFF;
    uint8_t dr = dst_px[dst->red_shift   / 8];
    uint8_t dg = dst_px[dst->green_shift / 8];
    uint8_t db = dst_px[dst->blue_shift  / 8];
    uint8_t inv = 255 - sa;
    dst_px[dst->red_shift   / 8] = (uint8_t)((sr * sa + dr * inv) / 255);
    dst_px[dst->green_shift / 8] = (uint8_t)((sg * sa + dg * inv) / 255);
    dst_px[dst->blue_shift  / 8] = (uint8_t)((sb * sa + db * inv) / 255);
    if (dst->bpp == 32)
        dst_px[stlxgfx_alpha_byte_index(dst)] = 0xFF;
}

int stlxgfx_fill_rect_blend(stlxgfx_surface_t* s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color) {
    if (!s || !s->pixels) {
        return -1;
    }

    uint8_t sa = (color >> 24) & 0xFF;
    if (sa == 0) {
        return 0;
    }
    if (sa == 255) {
        return stlxgfx_fill_rect(s, x, y, w, h, color);
    }

    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = (int32_t)(x + w);
    int32_t y1 = (int32_t)(y + h);
    if (x1 > (int32_t)s->width) {
        x1 = (int32_t)s->width;
    }
    if (y1 > (int32_t)s->height) {
        y1 = (int32_t)s->height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    uint32_t bytes_pp = s->bpp / 8;
    for (int32_t row = y0; row < y1; row++) {
        uint8_t* px = s->pixels + (uint32_t)row * s->pitch
                    + (uint32_t)x0 * bytes_pp;
        for (int32_t col = x0; col < x1; col++, px += bytes_pp) {
            blend_pixel(px, s, color);
        }
    }
    return 0;
}

void stlxgfx_blend_coverage(stlxgfx_surface_t* s, int32_t x, int32_t y,
                            uint32_t color, uint8_t coverage) {
    if (!s || !s->pixels || coverage == 0 ||
        x < 0 || y < 0 || x >= (int32_t)s->width || y >= (int32_t)s->height) {
        return;
    }

    uint32_t alpha = (((color >> 24) & 0xFF) * coverage) / 255;
    if (alpha == 0) {
        return;
    }

    uint8_t* px = s->pixels + (uint32_t)y * s->pitch
                + (uint32_t)x * (s->bpp / 8);
    blend_pixel(px, s, (alpha << 24) | (color & 0x00FFFFFF));
}

int stlxgfx_blit_alpha(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
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

    if ((uint32_t)sx + (uint32_t)sw > src->width) {
        sw = (int32_t)(src->width  - (uint32_t)sx);
    }
    if ((uint32_t)sy + (uint32_t)sh > src->height) {
        sh = (int32_t)(src->height - (uint32_t)sy);
    }
    if ((uint32_t)dx + (uint32_t)sw > dst->width) {
        sw = (int32_t)(dst->width  - (uint32_t)dx);
    }
    if ((uint32_t)dy + (uint32_t)sh > dst->height) {
        sh = (int32_t)(dst->height - (uint32_t)dy);
    }
    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    uint32_t dst_bpp = dst->bpp / 8;
    uint32_t src_bpp = src->bpp / 8;

    for (int32_t row = 0; row < sh; row++) {
        const uint8_t* src_row = src->pixels + ((uint32_t)sy + (uint32_t)row) * src->pitch + (uint32_t)sx * src_bpp;
        uint8_t* dst_row = dst->pixels + ((uint32_t)dy + (uint32_t)row) * dst->pitch + (uint32_t)dx * dst_bpp;
        for (int32_t col = 0; col < sw; col++) {
            uint32_t sc = read_pixel(src_row, src);
            blend_pixel(dst_row, dst, sc);
            src_row += src_bpp;
            dst_row += dst_bpp;
        }
    }
    return 0;
}

/* Edge coverage for a pixel at distance dist from an arc of radius r:
 * full inside, zero outside, one linear pixel across the boundary. */
static uint8_t blit_arc_coverage(float dist, float radius) {
    float c = radius + 0.5f - dist;
    if (c <= 0.0f) {
        return 0;
    }
    if (c >= 1.0f) {
        return 255;
    }
    return (uint8_t)(c * 255.0f + 0.5f);
}

int stlxgfx_blit_rounded_alpha(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                               const stlxgfx_surface_t* src,
                               int32_t sx, int32_t sy,
                               uint32_t w, uint32_t h, uint32_t radius) {
    if (!dst || !dst->pixels || !src || !src->pixels) {
        return -1;
    }
    uint32_t max_r = (w < h ? w : h) / 2;
    if (radius > max_r) {
        radius = max_r;
    }
    if (radius == 0) {
        return stlxgfx_blit_alpha(dst, dx, dy, src, sx, sy, w, h);
    }

    /* Corner arcs are anchored on the logical dest rect as passed in,
     * so clipping against the surface bounds below cannot shift them. */
    float r = (float)radius;
    float cx0 = (float)dx + r;
    float cy0 = (float)dy + r;
    float cx1 = (float)(dx + (int32_t)w) - r;
    float cy1 = (float)(dy + (int32_t)h) - r;

    int32_t sw = (int32_t)w;
    int32_t sh = (int32_t)h;

    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }
    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    if ((uint32_t)sx + (uint32_t)sw > src->width) {
        sw = (int32_t)(src->width  - (uint32_t)sx);
    }
    if ((uint32_t)sy + (uint32_t)sh > src->height) {
        sh = (int32_t)(src->height - (uint32_t)sy);
    }
    if ((uint32_t)dx + (uint32_t)sw > dst->width) {
        sw = (int32_t)(dst->width  - (uint32_t)dx);
    }
    if ((uint32_t)dy + (uint32_t)sh > dst->height) {
        sh = (int32_t)(dst->height - (uint32_t)dy);
    }
    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    uint32_t dst_bpp = dst->bpp / 8;
    uint32_t src_bpp = src->bpp / 8;

    for (int32_t row = 0; row < sh; row++) {
        const uint8_t* src_row = src->pixels + ((uint32_t)sy + (uint32_t)row) * src->pitch + (uint32_t)sx * src_bpp;
        uint8_t* dst_row = dst->pixels + ((uint32_t)dy + (uint32_t)row) * dst->pitch + (uint32_t)dx * dst_bpp;
        float py = (float)(dy + row) + 0.5f;
        for (int32_t col = 0; col < sw; col++) {
            float px = (float)(dx + col) + 0.5f;

            /* Nearest corner arc center; pixels in the straight bands
             * clamp to the edge and always get full coverage */
            float ax = px < cx0 ? cx0 : (px > cx1 ? cx1 : px);
            float ay = py < cy0 ? cy0 : (py > cy1 ? cy1 : py);
            uint8_t cov = 255;
            if (ax != px || ay != py) {
                float ddx = px - ax;
                float ddy = py - ay;
                cov = blit_arc_coverage(sqrtf(ddx * ddx + ddy * ddy), r);
            }

            if (cov) {
                uint32_t sc = read_pixel(src_row, src);
                uint32_t sa = ((sc >> 24) & 0xFF) * cov / 255;
                blend_pixel(dst_row, dst,
                            (sa << 24) | (sc & 0x00FFFFFF));
            }
            src_row += src_bpp;
            dst_row += dst_bpp;
        }
    }
    return 0;
}

int stlxgfx_blit_scaled(stlxgfx_surface_t* dst, int32_t dx, int32_t dy,
                        uint32_t dw, uint32_t dh,
                        const stlxgfx_surface_t* src, int32_t sx, int32_t sy,
                        uint32_t sw, uint32_t sh) {
    if (!dst || !dst->pixels || !src || !src->pixels) {
        return -1;
    }
    if (dw == 0 || dh == 0 || sw == 0 || sh == 0) {
        return 0;
    }
    if (sx < 0 || sy < 0 ||
        (uint32_t)sx + sw > src->width || (uint32_t)sy + sh > src->height) {
        return -1;
    }

    uint32_t dst_bpp = dst->bpp / 8;
    uint32_t src_bpp = src->bpp / 8;

    // 16.16 fixed point source stepping, sampling at pixel centers
    int64_t step_x = ((int64_t)sw << 16) / (int64_t)dw;
    int64_t step_y = ((int64_t)sh << 16) / (int64_t)dh;

    for (uint32_t oy = 0; oy < dh; oy++) {
        int32_t py = dy + (int32_t)oy;
        if (py < 0 || py >= (int32_t)dst->height) {
            continue;
        }

        int64_t fy = ((int64_t)oy * step_y) + (step_y >> 1) - (1 << 15);
        if (fy < 0) {
            fy = 0;
        }
        uint32_t y0 = (uint32_t)(fy >> 16);
        uint32_t y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        uint32_t wy = (uint32_t)(fy & 0xFFFF);

        const uint8_t* row0 = src->pixels
                            + ((uint32_t)sy + y0) * src->pitch
                            + (uint32_t)sx * src_bpp;
        const uint8_t* row1 = src->pixels
                            + ((uint32_t)sy + y1) * src->pitch
                            + (uint32_t)sx * src_bpp;
        uint8_t* out = dst->pixels + (uint32_t)py * dst->pitch;

        for (uint32_t ox = 0; ox < dw; ox++) {
            int32_t px = dx + (int32_t)ox;
            if (px < 0 || px >= (int32_t)dst->width) {
                continue;
            }

            int64_t fx = ((int64_t)ox * step_x) + (step_x >> 1) - (1 << 15);
            if (fx < 0) {
                fx = 0;
            }
            uint32_t x0 = (uint32_t)(fx >> 16);
            uint32_t x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            uint32_t wx = (uint32_t)(fx & 0xFFFF);

            uint32_t c00 = read_pixel(row0 + x0 * src_bpp, src);
            uint32_t c01 = read_pixel(row0 + x1 * src_bpp, src);
            uint32_t c10 = read_pixel(row1 + x0 * src_bpp, src);
            uint32_t c11 = read_pixel(row1 + x1 * src_bpp, src);

            uint32_t blended = 0;
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                uint32_t p00 = (c00 >> shift) & 0xFF;
                uint32_t p01 = (c01 >> shift) & 0xFF;
                uint32_t p10 = (c10 >> shift) & 0xFF;
                uint32_t p11 = (c11 >> shift) & 0xFF;
                uint32_t top = (p00 * (0x10000 - wx) + p01 * wx) >> 16;
                uint32_t bot = (p10 * (0x10000 - wx) + p11 * wx) >> 16;
                uint32_t val = (top * (0x10000 - wy) + bot * wy) >> 16;
                blended |= (val & 0xFF) << shift;
            }

            write_pixel(out + (uint32_t)px * dst_bpp, dst, blended);
        }
    }
    return 0;
}

void stlxgfx_blit_arc_corner(stlxgfx_surface_t* dst, int32_t x, int32_t y,
                             uint32_t r_outer, uint32_t r_inner,
                             int dir_x, int dir_y, int invert,
                             const stlxgfx_surface_t* src) {
    if (!dst || !dst->pixels || !src || !src->pixels || r_outer == 0) {
        return;
    }

    int32_t r = (int32_t)r_outer;
    float center_x = (float)x + (float)(dir_x * r);
    float center_y = (float)y + (float)(dir_y * r);

    uint32_t dst_bpp = dst->bpp / 8;
    uint32_t src_bpp = src->bpp / 8;

    for (int32_t oy = 0; oy < r; oy++) {
        for (int32_t ox = 0; ox < r; ox++) {
            int32_t px = x + (dir_x > 0 ? ox : -1 - ox);
            int32_t py = y + (dir_y > 0 ? oy : -1 - oy);
            if (px < 0 || py < 0 ||
                px >= (int32_t)dst->width || py >= (int32_t)dst->height ||
                px >= (int32_t)src->width || py >= (int32_t)src->height) {
                continue;
            }

            float ddx = ((float)px + 0.5f) - center_x;
            float ddy = ((float)py + 0.5f) - center_y;
            float dist = sqrtf(ddx * ddx + ddy * ddy);
            uint8_t cov_out = blit_arc_coverage(dist, (float)r);
            uint8_t cov;
            if (invert) {
                cov = (uint8_t)(255 - cov_out);
            } else if (r_inner > 0) {
                uint8_t cov_in = blit_arc_coverage(dist, (float)r_inner);
                cov = cov_out > cov_in ? (uint8_t)(cov_out - cov_in) : 0;
            } else {
                cov = cov_out;
            }
            if (cov == 0) {
                continue;
            }

            uint32_t sample = read_pixel(
                src->pixels + (uint32_t)py * src->pitch
                            + (uint32_t)px * src_bpp, src);
            blend_pixel(dst->pixels + (uint32_t)py * dst->pitch
                                    + (uint32_t)px * dst_bpp,
                        dst, (cov << 24) | (sample & 0x00FFFFFF));
        }
    }
}

int stlxgfx_draw_line(stlxgfx_surface_t* s, int32_t x0, int32_t y0,
                       int32_t x1, int32_t y1, uint32_t color) {
    if (!s || !s->pixels) {
        return -1;
    }

    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t abs_dx = dx < 0 ? -dx : dx;
    int32_t abs_dy = dy < 0 ? -dy : dy;
    int32_t sx = dx < 0 ? -1 : 1;
    int32_t sy = dy < 0 ? -1 : 1;

    uint32_t bytes_pp = s->bpp / 8;

    if (abs_dx >= abs_dy) {
        int32_t err = abs_dx / 2;
        int32_t y = y0;
        for (int32_t x = x0; x != x1 + sx; x += sx) {
            if (x >= 0 && x < (int32_t)s->width && y >= 0 && y < (int32_t)s->height) {
                uint8_t* px = s->pixels + (uint32_t)y * s->pitch + (uint32_t)x * bytes_pp;
                write_pixel(px, s, color);
            }
            err -= abs_dy;
            if (err < 0) {
                y += sy;
                err += abs_dx;
            }
        }
    } else {
        int32_t err = abs_dy / 2;
        int32_t x = x0;
        for (int32_t y = y0; y != y1 + sy; y += sy) {
            if (x >= 0 && x < (int32_t)s->width && y >= 0 && y < (int32_t)s->height) {
                uint8_t* px = s->pixels + (uint32_t)y * s->pitch + (uint32_t)x * bytes_pp;
                write_pixel(px, s, color);
            }
            err -= abs_dx;
            if (err < 0) {
                x += sx;
                err += abs_dy;
            }
        }
    }
    return 0;
}
