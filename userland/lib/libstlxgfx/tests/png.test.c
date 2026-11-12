/* PNG encoding, run on the build machine and read back through the decoder: opaque surfaces whatever their alpha
 * bytes hold, premultiplied pixels written in straight alpha, straight alpha pixels kept, and other depths refused */

#include <stlxgfx/image.h>
#include <stlxgfx/surface.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH       7
#define HEIGHT      5
#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;

static void check(int holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static uint32_t pixel_at(const stlxgfx_surface_t* s, uint32_t x, uint32_t y) {
    uint32_t pixel;
    memcpy(&pixel, s->pixels + (size_t)y * s->pitch + (size_t)x * 4, sizeof(pixel));
    return pixel;
}

static void set_pixel(stlxgfx_surface_t* s, uint32_t x, uint32_t y, uint32_t argb) {
    memcpy(s->pixels + (size_t)y * s->pitch + (size_t)x * 4, &argb, sizeof(argb));
}

/* A surface of distinct colors no brighter than their alpha, each pixel's alpha from `alphas` in turn */
static stlxgfx_surface_t* new_surface(uint32_t flags, const uint32_t* alphas, size_t alpha_count) {
    stlxgfx_surface_t* s = stlxgfx_create_surface(WIDTH, HEIGHT, 32, 16, 8, 0);
    if (!s) {
        fprintf(stderr, "png: out of memory\n");
        exit(1);
    }

    s->flags = flags;
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t alpha = alphas[(y * WIDTH + x) % alpha_count];
            uint32_t red = (x * 37 + y * 11) % (alpha + 1);
            uint32_t green = (x * 5 + y * 53) % (alpha + 1);
            uint32_t blue = (x * 91 + y * 7) % (alpha + 1);
            set_pixel(s, x, y, alpha << 24 | red << 16 | green << 8 | blue);
        }
    }

    return s;
}

static stlxgfx_surface_t* round_trip(const stlxgfx_surface_t* s, int keeps_alpha) {
    size_t len = 0;
    unsigned char* png = stlxgfx_encode_png(s, keeps_alpha, &len);
    check(png != NULL && len > 8 && memcmp(png, "\x89PNG", 4) == 0, "an encoded surface starts with the PNG signature");
    if (!png) {
        return NULL;
    }

    stlxgfx_surface_t* decoded = stlxgfx_decode_image(png, len);
    free(png);
    check(decoded != NULL && decoded->width == WIDTH && decoded->height == HEIGHT, "the PNG decodes at the same size");
    return decoded;
}

static uint32_t straight_channel(uint32_t channel, uint32_t alpha) {
    uint32_t value = (channel * 255 + alpha / 2) / alpha;
    return value > 255 ? 255 : value;
}

static void check_opaque(void) {
    const uint32_t alphas[] = { 0, 17, 255 };
    stlxgfx_surface_t* s = new_surface(0, alphas, 3);
    stlxgfx_surface_t* decoded = round_trip(s, 0);
    int is_same = decoded != NULL;
    for (uint32_t y = 0; decoded && y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            is_same = is_same && pixel_at(decoded, x, y) == (0xFF000000u | (pixel_at(s, x, y) & 0x00FFFFFFu));
        }
    }

    check(is_same, "an opaque encode keeps every color and writes alpha 255 whatever the alpha bytes held");
    stlxgfx_destroy_surface(decoded);
    stlxgfx_destroy_surface(s);
}

static void check_premultiplied(void) {
    const uint32_t alphas[] = { 0, 51, 128, 200, 255 };
    stlxgfx_surface_t* s = new_surface(STLXGFX_SURFACE_PREMULTIPLIED, alphas, 5);
    stlxgfx_surface_t* decoded = round_trip(s, 1);
    int is_straight = decoded != NULL;
    for (uint32_t y = 0; decoded && y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t pixel = pixel_at(s, x, y);
            uint32_t alpha = pixel >> 24;
            uint32_t want = alpha << 24;
            for (uint32_t shift = 0; alpha > 0 && shift < 24; shift += 8) {
                uint32_t channel = (pixel >> shift) & 0xFF;
                want |= (alpha == 255 ? channel : straight_channel(channel, alpha)) << shift;
            }

            is_straight = is_straight && pixel_at(decoded, x, y) == want;
        }
    }

    check(is_straight, "premultiplied pixels are written in straight alpha, transparent ones as zero");
    stlxgfx_destroy_surface(decoded);
    stlxgfx_destroy_surface(s);
}

static void check_straight(void) {
    const uint32_t alphas[] = { 9, 99, 255 };
    stlxgfx_surface_t* s = new_surface(STLXGFX_SURFACE_STRAIGHT_ALPHA, alphas, 3);
    stlxgfx_surface_t* decoded = round_trip(s, 1);
    int is_same = decoded != NULL;
    for (uint32_t y = 0; decoded && y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            is_same = is_same && pixel_at(decoded, x, y) == pixel_at(s, x, y);
        }
    }

    check(is_same, "straight alpha pixels are written as they are");
    stlxgfx_destroy_surface(decoded);
    stlxgfx_destroy_surface(s);
}

static void check_refusals(void) {
    stlxgfx_surface_t* s = stlxgfx_create_surface(WIDTH, HEIGHT, 32, 16, 8, 0);
    size_t len = 0;
    if (s) {
        s->bpp = 16;
    }

    check(s && stlxgfx_encode_png(s, 0, &len) == NULL, "a surface of another depth is refused");
    check(stlxgfx_encode_png(NULL, 0, &len) == NULL, "no surface is refused");
    stlxgfx_destroy_surface(s);
}

int main(void) {
    check_opaque();
    check_premultiplied();
    check_straight();
    check_refusals();

    if (g_failures != 0) {
        printf("png: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("png: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
