/* gfxbench: raster loop benchmarks run on the target itself.
 *
 * Host-side -O2 numbers are misleading for Stellux userland because the
 * deployment target is often QEMU TCG, where vectorized code lowers to
 * helper calls and scalar packed loops win. This app times the blend
 * variants on a full 1920x1080 frame in the real environment and prints
 * ns/pixel, so raster changes are judged on the target, not the host.
 * Run from the serial shell, it takes a few seconds.
 */
#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/surface.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_W 1920u
#define BENCH_H 1080u
#define BENCH_RUNS 5
#define DIM_COLOR 0xA0000000u

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Variant A: the pre-repair library shape, per-pixel offset loads
 * through the surface pointer and /255 per channel. */
static void fill_old_shape(stlxgfx_surface_t* s, uint32_t color) {
    uint8_t sa = (color >> 24) & 0xFF;
    uint8_t sr = (color >> 16) & 0xFF;
    uint8_t sg = (color >>  8) & 0xFF;
    uint8_t sb =  color        & 0xFF;
    uint32_t bytes_pp = s->bpp / 8;

    for (uint32_t row = 0; row < s->height; row++) {
        uint8_t* px = s->pixels + (size_t)row * s->pitch;
        for (uint32_t col = 0; col < s->width; col++, px += bytes_pp) {
            uint8_t dr = px[s->red_shift   / 8];
            uint8_t dg = px[s->green_shift / 8];
            uint8_t db = px[s->blue_shift  / 8];
            uint8_t inv = 255 - sa;
            px[s->red_shift   / 8] = (uint8_t)((sr * sa + dr * inv) / 255);
            px[s->green_shift / 8] = (uint8_t)((sg * sa + dg * inv) / 255);
            px[s->blue_shift  / 8] = (uint8_t)((sb * sa + db * inv) / 255);
            px[stlxgfx_alpha_byte_index(s)] = 0xFF;
        }
    }
}

/* Variant B: whole-pixel packed loop, channels extracted by bit shift,
 * plain /255 for the compiler to strength-reduce. */
static void fill_packed_div(stlxgfx_surface_t* s, uint32_t color) {
    uint32_t sa = (color >> 24) & 0xFF;
    uint32_t inv = 255 - sa;
    uint32_t rs = s->red_shift, gs = s->green_shift, bs = s->blue_shift;
    uint32_t as = (uint32_t)stlxgfx_alpha_byte_index(s) * 8;
    uint32_t spr = ((color >> 16) & 0xFF) * sa;
    uint32_t spg = ((color >>  8) & 0xFF) * sa;
    uint32_t spb = ( color        & 0xFF) * sa;

    for (uint32_t row = 0; row < s->height; row++) {
        uint32_t* px = (uint32_t*)(void*)(s->pixels + (size_t)row * s->pitch);
        for (uint32_t col = 0; col < s->width; col++) {
            uint32_t d = px[col];
            uint32_t r = (spr + ((d >> rs) & 0xFF) * inv) / 255;
            uint32_t g = (spg + ((d >> gs) & 0xFF) * inv) / 255;
            uint32_t b = (spb + ((d >> bs) & 0xFF) * inv) / 255;
            px[col] = (r << rs) | (g << gs) | (b << bs) | (0xFFu << as);
        }
    }
}

/* Variant C: packed loop with the shift-based exact /255. */
static inline uint32_t div255(uint32_t x) {
    return (x + 1 + (x >> 8)) >> 8;
}

static void fill_packed_shift(stlxgfx_surface_t* s, uint32_t color) {
    uint32_t sa = (color >> 24) & 0xFF;
    uint32_t inv = 255 - sa;
    uint32_t rs = s->red_shift, gs = s->green_shift, bs = s->blue_shift;
    uint32_t as = (uint32_t)stlxgfx_alpha_byte_index(s) * 8;
    uint32_t spr = ((color >> 16) & 0xFF) * sa;
    uint32_t spg = ((color >>  8) & 0xFF) * sa;
    uint32_t spb = ( color        & 0xFF) * sa;

    for (uint32_t row = 0; row < s->height; row++) {
        uint32_t* px = (uint32_t*)(void*)(s->pixels + (size_t)row * s->pitch);
        for (uint32_t col = 0; col < s->width; col++) {
            uint32_t d = px[col];
            uint32_t r = div255(spr + ((d >> rs) & 0xFF) * inv);
            uint32_t g = div255(spg + ((d >> gs) & 0xFF) * inv);
            uint32_t b = div255(spb + ((d >> bs) & 0xFF) * inv);
            px[col] = (r << rs) | (g << gs) | (b << bs) | (0xFFu << as);
        }
    }
}

typedef void (*fill_fn)(stlxgfx_surface_t*, uint32_t);

static void bench_one(const char* name, fill_fn fn, stlxgfx_surface_t* s,
                      const uint8_t* orig, size_t bytes) {
    uint64_t best = ~0ull;
    for (int run = 0; run < BENCH_RUNS; run++) {
        memcpy(s->pixels, orig, bytes);
        uint64_t t0 = now_ns();
        fn(s, DIM_COLOR);
        uint64_t t1 = now_ns();
        if (t1 - t0 < best) {
            best = t1 - t0;
        }
    }

    double px = (double)BENCH_W * BENCH_H;
    printf("%-14s %7.2f ms  %6.2f ns/px\r\n",
           name, (double)best / 1e6, (double)best / px);
}

static void bench_lib(stlxgfx_surface_t* s, const uint8_t* orig,
                      size_t bytes) {
    uint64_t best = ~0ull;
    for (int run = 0; run < BENCH_RUNS; run++) {
        memcpy(s->pixels, orig, bytes);
        uint64_t t0 = now_ns();
        stlxgfx_fill_rect_blend(s, 0, 0, BENCH_W, BENCH_H, DIM_COLOR);
        uint64_t t1 = now_ns();
        if (t1 - t0 < best) {
            best = t1 - t0;
        }
    }

    double px = (double)BENCH_W * BENCH_H;
    printf("%-14s %7.2f ms  %6.2f ns/px\r\n",
           "lib", (double)best / 1e6, (double)best / px);
}

int main(void) {
    size_t bytes = (size_t)BENCH_W * BENCH_H * 4;
    uint8_t* orig = malloc(bytes);
    stlxgfx_surface_t* s = stlxgfx_create_surface(BENCH_W, BENCH_H, 32,
                                                  16, 8, 0);
    if (!orig || !s) {
        printf("gfxbench: allocation failed\r\n");
        return 1;
    }

    srand(1234);
    for (size_t i = 0; i < bytes; i++) {
        orig[i] = (uint8_t)rand();
    }

    /* Correctness gate before timing: all variants must agree with the
     * old shape byte for byte on a random frame. */
    uint8_t* ref = malloc(bytes);
    if (!ref) {
        printf("gfxbench: allocation failed\r\n");
        return 1;
    }

    memcpy(s->pixels, orig, bytes);
    fill_old_shape(s, DIM_COLOR);
    memcpy(ref, s->pixels, bytes);

    fill_fn variants[] = { fill_packed_div, fill_packed_shift };
    const char* names[] = { "packed-div", "packed-shift" };
    for (int i = 0; i < 2; i++) {
        memcpy(s->pixels, orig, bytes);
        variants[i](s, DIM_COLOR);
        if (memcmp(ref, s->pixels, bytes) != 0) {
            printf("gfxbench: %s output MISMATCH\r\n", names[i]);
            return 1;
        }
    }

    memcpy(s->pixels, orig, bytes);
    stlxgfx_fill_rect_blend(s, 0, 0, BENCH_W, BENCH_H, DIM_COLOR);
    if (memcmp(ref, s->pixels, bytes) != 0) {
        printf("gfxbench: lib output MISMATCH\r\n");
        return 1;
    }
    printf("gfxbench: all variants byte-identical, timing...\r\n");

    bench_one("old-shape", fill_old_shape, s, orig, bytes);
    bench_lib(s, orig, bytes);
    bench_one("packed-div", fill_packed_div, s, orig, bytes);
    bench_one("packed-shift", fill_packed_shift, s, orig, bytes);

    printf("gfxbench: done\r\n");
    free(ref);
    free(orig);
    stlxgfx_destroy_surface(s);
    return 0;
}
