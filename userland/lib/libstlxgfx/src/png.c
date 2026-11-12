#include <stlxgfx/image.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stlxgfx/internal/stb_image_write.h>
#pragma GCC diagnostic pop

/* A premultiplied channel as straight alpha, rounded to nearest */
static uint8_t straight_channel(uint32_t channel, uint32_t alpha) {
    uint32_t value = (channel * 255 + alpha / 2) / alpha;
    return (uint8_t)(value > 255 ? 255 : value);
}

/* stb_image_write takes rows of R, G, B and optionally A bytes, so the surface's layout is rewritten into them first */
unsigned char* stlxgfx_encode_png(const stlxgfx_surface_t* surface, int keeps_alpha, size_t* out_len) {
    if (!surface || !out_len || surface->bpp != 32 || surface->width == 0 || surface->height == 0) {
        return NULL;
    }

    size_t channels = keeps_alpha ? 4 : 3;
    size_t row_len = (size_t)surface->width * channels;
    if (row_len > INT32_MAX / surface->height) {
        return NULL;
    }

    unsigned char* rows = malloc(row_len * surface->height);
    if (!rows) {
        return NULL;
    }

    uint32_t alpha_shift = stlxgfx_alpha_byte_index(surface) * 8u;
    int is_premultiplied = (surface->flags & STLXGFX_SURFACE_PREMULTIPLIED) != 0;
    for (uint32_t y = 0; y < surface->height; y++) {
        const uint8_t* src = surface->pixels + (size_t)y * surface->pitch;
        unsigned char* dst = rows + (size_t)y * row_len;
        for (uint32_t x = 0; x < surface->width; x++, dst += channels) {
            uint32_t pixel;
            memcpy(&pixel, src + (size_t)x * 4, sizeof(pixel));
            uint32_t red = (pixel >> surface->red_shift) & 0xFF;
            uint32_t green = (pixel >> surface->green_shift) & 0xFF;
            uint32_t blue = (pixel >> surface->blue_shift) & 0xFF;
            uint32_t alpha = (pixel >> alpha_shift) & 0xFF;

            if (keeps_alpha && is_premultiplied && alpha < 255) {
                red = alpha ? straight_channel(red, alpha) : 0;
                green = alpha ? straight_channel(green, alpha) : 0;
                blue = alpha ? straight_channel(blue, alpha) : 0;
            }

            dst[0] = (unsigned char)red;
            dst[1] = (unsigned char)green;
            dst[2] = (unsigned char)blue;
            if (keeps_alpha) {
                dst[3] = (unsigned char)alpha;
            }
        }
    }

    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(rows, (int)row_len, (int)surface->width, (int)surface->height,
                                               (int)channels, &len);
    free(rows);
    if (!png) {
        return NULL;
    }

    *out_len = (size_t)len;
    return png;
}
