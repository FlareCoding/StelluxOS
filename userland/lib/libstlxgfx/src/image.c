#include <stlxgfx/image.h>
#include <stlxgfx/bmp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stlxgfx/internal/stb_image.h>
#pragma GCC diagnostic pop

/* Takes stb_image's RGBA rows into a surface with the BMP loader's layout, 32 bits in byte order B, G, R, A, and frees
 * them */
static stlxgfx_surface_t* surface_from_rgba(unsigned char* pixels, int width, int height) {
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return NULL;
    }

    stlxgfx_surface_t* surface = stlxgfx_create_surface(
        (uint32_t)width, (uint32_t)height, 32, 16, 8, 0);
    if (!surface) {
        stbi_image_free(pixels);
        return NULL;
    }

    for (int y = 0; y < height; y++) {
        const unsigned char* src = pixels + (size_t)y * (size_t)width * 4;
        uint8_t* dst = surface->pixels + (uint32_t)y * surface->pitch;
        for (int x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    stbi_image_free(pixels);
    return surface;
}

stlxgfx_surface_t* stlxgfx_decode_image(const void* data, size_t len) {
    if (!data || len == 0 || len > INT32_MAX) {
        return NULL;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 4);
    if (!pixels) {
        return NULL;
    }

    return surface_from_rgba(pixels, width, height);
}

stlxgfx_surface_t* stlxgfx_load_image(const char* path) {
    if (!path) {
        return NULL;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    unsigned char magic[2] = {0};
    size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (got != sizeof(magic)) {
        return NULL;
    }

    /* The BMP loader understands the alpha conventions the BMP assets
     * rely on, so BMP files go through it instead of stb_image */
    if (magic[0] == 'B' && magic[1] == 'M') {
        return stlxgfx_load_bmp(path);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels) {
        return NULL;
    }

    return surface_from_rgba(pixels, width, height);
}
