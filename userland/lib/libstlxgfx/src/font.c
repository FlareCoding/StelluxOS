#include <stlxgfx/font.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define STB_TRUETYPE_IMPLEMENTATION
#include <stlxgfx/internal/stb_truetype.h>
#pragma GCC diagnostic pop

#define CHAR_CACHE_SIZE 128
#define CHAR_CACHE_BASE 32

typedef struct {
    unsigned char* bitmap;
    int width;
    int height;
    int xoff;
    int yoff;
    uint32_t font_size;
    int valid;
} char_cache_entry_t;

static stbtt_fontinfo g_font_info;
static void*          g_font_data;
static int            g_font_loaded;
static char_cache_entry_t g_cache[CHAR_CACHE_SIZE];
static uint32_t       g_cached_font_size;

static void cache_clear(void) {
    for (int i = 0; i < CHAR_CACHE_SIZE; i++) {
        if (g_cache[i].bitmap) {
            free(g_cache[i].bitmap);
            g_cache[i].bitmap = NULL;
        }
        g_cache[i].valid = 0;
    }
    g_cached_font_size = 0;
}

static unsigned char* cache_get(int codepoint, uint32_t font_size,
                                int* w, int* h, int* xoff, int* yoff) {
    if (codepoint < CHAR_CACHE_BASE ||
        codepoint >= CHAR_CACHE_BASE + CHAR_CACHE_SIZE) {
        return NULL;
    }

    if (g_cached_font_size != font_size) {
        cache_clear();
        g_cached_font_size = font_size;
    }

    int idx = codepoint - CHAR_CACHE_BASE;
    char_cache_entry_t* e = &g_cache[idx];

    if (e->valid && e->font_size == font_size) {
        *w = e->width;
        *h = e->height;
        *xoff = e->xoff;
        *yoff = e->yoff;
        return e->bitmap;
    }

    float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)font_size);
    unsigned char* bmp = stbtt_GetCodepointBitmap(
        &g_font_info, scale, scale, codepoint, w, h, xoff, yoff);
    if (!bmp || *w <= 0 || *h <= 0) {
        return NULL;
    }

    size_t bmp_size = (size_t)(*w) * (size_t)(*h);
    e->bitmap = malloc(bmp_size);
    if (e->bitmap) {
        memcpy(e->bitmap, bmp, bmp_size);
        e->width = *w;
        e->height = *h;
        e->xoff = *xoff;
        e->yoff = *yoff;
        e->font_size = font_size;
        e->valid = 1;
    }

    stbtt_FreeBitmap(bmp, NULL);
    return e->bitmap;
}

int stlxgfx_font_init(const char* font_path) {
    if (!font_path) {
        return -1;
    }
    if (g_font_loaded) {
        return 0;
    }

    int fd = open(font_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    off_t size = st.st_size;

    void* data = malloc((size_t)size);
    if (!data) {
        close(fd);
        return -1;
    }

    ssize_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, (char*)data + total, (size_t)(size - total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    close(fd);

    if (total != size) {
        free(data);
        return -1;
    }

    if (!stbtt_InitFont(&g_font_info, data, 0)) {
        free(data);
        return -1;
    }

    g_font_data = data;
    g_font_loaded = 1;

    return 0;
}

void stlxgfx_font_cleanup(void) {
    cache_clear();
    if (g_font_data) {
        free(g_font_data);
        g_font_data = NULL;
    }
    g_font_loaded = 0;
}

static inline void alpha_blend_pixel(uint8_t* pixel, const stlxgfx_surface_t* s,
                                     uint32_t color, uint8_t alpha) {
    if (alpha == 0) {
        return;
    }

    uint8_t src_r = (color >> 16) & 0xFF;
    uint8_t src_g = (color >>  8) & 0xFF;
    uint8_t src_b =  color        & 0xFF;

    uint32_t bytes_pp = s->bpp / 8;

    if (alpha == 255) {
        pixel[s->red_shift   / 8] = src_r;
        pixel[s->green_shift / 8] = src_g;
        pixel[s->blue_shift  / 8] = src_b;
        if (bytes_pp == 4) {
            uint8_t alpha_byte = 3;
            if (s->red_shift != 0 && s->green_shift != 0 && s->blue_shift != 0) {
                alpha_byte = 0;
            }
            pixel[alpha_byte] = 0xFF;
        }
        return;
    }

    uint8_t dst_r = pixel[s->red_shift   / 8];
    uint8_t dst_g = pixel[s->green_shift / 8];
    uint8_t dst_b = pixel[s->blue_shift  / 8];

    uint8_t inv = 255 - alpha;
    uint8_t out_r = (uint8_t)((src_r * alpha + dst_r * inv) / 255);
    uint8_t out_g = (uint8_t)((src_g * alpha + dst_g * inv) / 255);
    uint8_t out_b = (uint8_t)((src_b * alpha + dst_b * inv) / 255);

    pixel[s->red_shift   / 8] = out_r;
    pixel[s->green_shift / 8] = out_g;
    pixel[s->blue_shift  / 8] = out_b;
    if (bytes_pp == 4) {
        uint8_t alpha_byte = 3;
        if (s->red_shift != 0 && s->green_shift != 0 && s->blue_shift != 0) {
            alpha_byte = 0;
        }
        pixel[alpha_byte] = 0xFF;
    }
}

int stlxgfx_draw_text(stlxgfx_surface_t* s, int32_t x, int32_t y,
                      const char* text, uint32_t font_size, uint32_t color) {
    if (!s || !s->pixels || !text || !g_font_loaded || font_size == 0) {
        return -1;
    }

    float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)font_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&g_font_info, &ascent, &descent, &line_gap);
    int baseline_y = y + (int)((float)ascent * scale);

    int32_t current_x = x;
    uint32_t bytes_pp = s->bpp / 8;

    for (int i = 0; text[i]; i++) {
        int codepoint = (unsigned char)text[i];
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font_info, codepoint, &advance, &lsb);

        int char_w, char_h, xoff, yoff;
        unsigned char* bitmap = cache_get(codepoint, font_size,
                                          &char_w, &char_h, &xoff, &yoff);
        int should_free = 0;
        if (!bitmap) {
            bitmap = stbtt_GetCodepointBitmap(
                &g_font_info, scale, scale, codepoint,
                &char_w, &char_h, &xoff, &yoff);
            should_free = 1;
        }

        if (bitmap && char_w > 0 && char_h > 0) {
            int32_t gx = current_x + xoff;
            int32_t gy = baseline_y + yoff;

            for (int py = 0; py < char_h; py++) {
                int32_t sy = gy + py;
                if (sy < 0 || sy >= (int32_t)s->height) {
                    continue;
                }
                for (int px = 0; px < char_w; px++) {
                    int32_t sx = gx + px;
                    if (sx < 0 || sx >= (int32_t)s->width) {
                        continue;
                    }

                    uint8_t coverage = bitmap[py * char_w + px];
                    if (coverage > 0) {
                        uint8_t* pixel = s->pixels + (uint32_t)sy * s->pitch
                                       + (uint32_t)sx * bytes_pp;
                        alpha_blend_pixel(pixel, s, color, coverage);
                    }
                }
            }
        }

        if (should_free && bitmap) {
            stbtt_FreeBitmap(bitmap, NULL);
        }

        current_x += (int32_t)((float)advance * scale);
    }

    return 0;
}

void stlxgfx_text_size(const char* text, uint32_t font_size,
                       uint32_t* out_w, uint32_t* out_h) {
    if (!text || !g_font_loaded || font_size == 0) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)font_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&g_font_info, &ascent, &descent, &line_gap);

    int width = 0;
    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font_info, (unsigned char)text[i],
                                   &advance, &lsb);
        width += (int)((float)advance * scale);
    }

    if (out_w) *out_w = (uint32_t)(width > 0 ? width : 0);
    if (out_h) *out_h = (uint32_t)((int)((float)(ascent - descent) * scale));
}
