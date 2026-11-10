/* Fills and lines onto premultiplied surfaces, and the blending calls `main` lists over transparent, partly
 * covered and opaque pixels, run on the build machine and checked against the rule restated here */

#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxtheme/theme.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANNEL_VALUES 256
#define SWEEP_WIDTH    256
#define SWEEP_HEIGHT   3
#define SHAPE_SIZE     64
#define TEXT_WIDTH     160
#define TEXT_HEIGHT    32
#define FONT_PX        20
#define MAX_REPORTS    8
#define OPAQUE         0xFF000000u
#define TRANSLUCENT    0x80000000u
#define WHITE          0x00FFFFFFu
#define LINE_COLOR     0x20C0FFu
#define BELOW_COLOR    0x4080C0u

#define GRADIENT_COLUMN_STEP 4
#define GRADIENT_ROW_STEP    3
#define ROUNDED_RADIUS       20
#define ARC_OUTER_RADIUS     48
#define ARC_INNER_RADIUS     20
#define TEXT_PEN_X           2
#define TEXT_BASELINE        24
#define FILL_BLUE_STEP       7

/* One blending call, drawing color `rgb` at the alpha each pixel gets from the call's geometry or source,
 * each pixel at most once, since two draws stacked and blended once round differently */
typedef struct {
    const char* name;
    uint32_t    width;
    uint32_t    height;
    void      (*draw)(stlxgfx_surface_t* dst, uint32_t rgb);
} draw_call;

static stlxgfx_font* g_font;
static uint64_t      g_checks;
static uint64_t      g_failures;

/* A 32 bpp surface with `flags`, cleared to `argb`. The test exits when memory runs out. */
static stlxgfx_surface_t* new_surface(uint32_t width, uint32_t height, uint32_t flags, uint32_t argb) {
    stlxgfx_surface_t* s = stlxgfx_create_surface(width, height, 32, 16, 8, 0);
    if (!s) {
        fprintf(stderr, "premultiplied: out of memory\n");
        exit(1);
    }

    s->flags = flags;
    stlxgfx_clear(s, argb);

    return s;
}

/* A straight alpha source whose alpha varies along both rows and columns */
static stlxgfx_surface_t* new_gradient(uint32_t width, uint32_t height, uint32_t rgb) {
    stlxgfx_surface_t* src = new_surface(width, height, 0, 0);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t alpha = (x * GRADIENT_COLUMN_STEP + y * GRADIENT_ROW_STEP) & 0xFF;
            stlxgfx_fill_rect(src, (int32_t)x, (int32_t)y, 1, 1, (alpha << 24) | rgb);
        }
    }

    return src;
}

/* The pixel at (`x`, `y`) as ARGB */
static uint32_t pixel_at(const stlxgfx_surface_t* s, uint32_t x, uint32_t y) {
    const uint8_t* p = s->pixels + y * s->pitch + x * 4;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* `argb` blended onto every row at the coverage of each column, 0 to 255 */
static void sweep_blend_coverage(stlxgfx_surface_t* dst, uint32_t argb) {
    for (uint32_t y = 0; y < SWEEP_HEIGHT; y++) {
        for (uint32_t x = 0; x < SWEEP_WIDTH; x++) {
            stlxgfx_blend_coverage(dst, (int32_t)x, (int32_t)y, argb, (uint8_t)x);
        }
    }
}

/* The test's line of text in `argb` */
static void write_text(stlxgfx_surface_t* dst, uint32_t argb) {
    static const char text[] = "Stellux 0.1";
    stlxgfx_draw_text(dst, g_font, TEXT_PEN_X, TEXT_BASELINE, text, strlen(text), argb);
}

/* The calls under test, each drawing `rgb` at alphas that vary across its pixels */
static void draw_blend_coverage(stlxgfx_surface_t* dst, uint32_t rgb) {
    sweep_blend_coverage(dst, OPAQUE | rgb);
}

static void draw_translucent_coverage(stlxgfx_surface_t* dst, uint32_t rgb) {
    sweep_blend_coverage(dst, TRANSLUCENT | rgb);
}

static void draw_fill_rect_blend(stlxgfx_surface_t* dst, uint32_t rgb) {
    for (uint32_t x = 0; x < SWEEP_WIDTH; x++) {
        stlxgfx_fill_rect_blend(dst, (int32_t)x, 0, 1, SWEEP_HEIGHT, (x << 24) | rgb);
    }
}

static void draw_blit_alpha(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_surface(SWEEP_WIDTH, SWEEP_HEIGHT, 0, 0);
    for (uint32_t x = 0; x < SWEEP_WIDTH; x++) {
        stlxgfx_fill_rect(src, (int32_t)x, 0, 1, SWEEP_HEIGHT, (x << 24) | rgb);
    }

    stlxgfx_blit_alpha(dst, 0, 0, src, 0, 0, SWEEP_WIDTH, SWEEP_HEIGHT);
    stlxgfx_destroy_surface(src);
}

static void draw_blit_rounded_alpha(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_gradient(SHAPE_SIZE, SHAPE_SIZE, rgb);
    stlxgfx_blit_rounded_alpha(dst, 0, 0, src, 0, 0, SHAPE_SIZE, SHAPE_SIZE, ROUNDED_RADIUS);
    stlxgfx_destroy_surface(src);
}

static void draw_blit_arc_corner(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_surface(SHAPE_SIZE, SHAPE_SIZE, 0, OPAQUE | rgb);
    stlxgfx_blit_arc_corner(dst, 0, 0, ARC_OUTER_RADIUS, ARC_INNER_RADIUS, 1, 1, 0, src);
    stlxgfx_destroy_surface(src);
}

static void draw_text(stlxgfx_surface_t* dst, uint32_t rgb) {
    write_text(dst, OPAQUE | rgb);
}

static void draw_translucent_text(stlxgfx_surface_t* dst, uint32_t rgb) {
    write_text(dst, TRANSLUCENT | rgb);
}

/* Counts one check and reports the first MAX_REPORTS failures */
static void expect(
    int holds,
    const char* call,
    const char* what,
    uint32_t x,
    uint32_t y,
    uint32_t got,
    uint32_t want
) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s, %s at (%u, %u): 0x%08X, expected 0x%08X\n", call, what, x, y, got, want);
    }
}

/* Whether no channel of the premultiplied `pixel` exceeds its alpha */
static int channels_within_alpha(uint32_t pixel) {
    uint32_t alpha = pixel >> 24;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        if (((pixel >> shift) & 0xFF) > alpha) {
            return 0;
        }
    }

    return 1;
}

/* A fill of `argb` stores each channel times its alpha, rounded to the nearest value */
static uint32_t premultiplied(uint32_t argb) {
    uint32_t a = argb >> 24;
    uint32_t stored = a << 24;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t c = (argb >> shift) & 0xFF;
        stored |= ((2 * c * a + 255) / 510) << shift;
    }

    return stored;
}

/* `c` drawn at alpha `s` onto the premultiplied `below` stores the coverage of both as its alpha, and in each channel
 * `c` weighted by `s` plus `below` weighted by 255 minus `s`, both truncated */
static uint32_t premultiplied_over(uint32_t below, uint32_t c, uint32_t s) {
    uint32_t d = below >> 24;
    uint32_t over = (s + d * (255 - s) / 255) << 24;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t drawn = (c >> shift) & 0xFF;
        uint32_t kept = (below >> shift) & 0xFF;
        over |= ((drawn * s + kept * (255 - s)) / 255) << shift;
    }

    return over;
}

/* Every channel value at every alpha, each channel in its own order, fills as its premultiplied color */
static void check_fills(void) {
    stlxgfx_surface_t* s = new_surface(CHANNEL_VALUES, CHANNEL_VALUES, STLXGFX_SURFACE_PREMULTIPLIED, 0);
    for (uint32_t a = 0; a < CHANNEL_VALUES; a++) {
        for (uint32_t c = 0; c < CHANNEL_VALUES; c++) {
            uint32_t argb = (a << 24) | (c << 16) | ((255 - c) << 8) | ((c * FILL_BLUE_STEP) & 0xFF);
            stlxgfx_fill_rect(s, (int32_t)c, (int32_t)a, 1, 1, argb);

            uint32_t got = pixel_at(s, c, a);
            uint32_t want = premultiplied(argb);
            expect(got == want, "fill_rect", "stored color", c, a, got, want);
            expect(channels_within_alpha(got), "fill_rect", "channels within alpha", c, a, got, want);
        }
    }

    stlxgfx_destroy_surface(s);
}

/* A line at every alpha stores its color premultiplied in every pixel it draws */
static void check_lines(void) {
    stlxgfx_surface_t* s = new_surface(CHANNEL_VALUES, CHANNEL_VALUES, STLXGFX_SURFACE_PREMULTIPLIED, 0);
    for (uint32_t a = 0; a < CHANNEL_VALUES; a++) {
        uint32_t argb = (a << 24) | LINE_COLOR;
        stlxgfx_draw_line(s, 0, (int32_t)a, CHANNEL_VALUES - 1, (int32_t)a, argb);

        for (uint32_t x = 0; x < CHANNEL_VALUES; x++) {
            uint32_t got = pixel_at(s, x, a);
            expect(got == premultiplied(argb), "draw_line", "stored color", x, a, got, premultiplied(argb));
        }
    }

    stlxgfx_destroy_surface(s);
}

/* Onto transparent pixels each pixel stores the color at the alpha the call gives it, and some pixel is partly
 * covered */
static void check_single_draws(const draw_call* call, const stlxgfx_surface_t* picture, uint32_t rgb) {
    uint32_t partial = 0;

    for (uint32_t y = 0; y < call->height; y++) {
        for (uint32_t x = 0; x < call->width; x++) {
            uint32_t got = pixel_at(picture, x, y);
            uint32_t s = got >> 24;
            uint32_t want = premultiplied_over(0, rgb, s);
            expect(got == want, call->name, "single draw", x, y, got, want);
            partial += s != 0 && s != 255;
        }
    }

    g_checks++;
    if (partial == 0) {
        g_failures++;
        printf("  FAIL: %s, no partly covered pixel drawn\n", call->name);
    }
}

/* Over pixels cleared to `argb` a draw follows the rule at the alpha it gave each pixel of `picture` and keeps every
 * channel within the alpha */
static void check_stacked_draws(const draw_call* call, const stlxgfx_surface_t* picture, uint32_t rgb, uint32_t argb) {
    stlxgfx_surface_t* stacked = new_surface(call->width, call->height, STLXGFX_SURFACE_PREMULTIPLIED, argb);
    uint32_t below = pixel_at(stacked, 0, 0);

    call->draw(stacked, rgb);

    for (uint32_t y = 0; y < call->height; y++) {
        for (uint32_t x = 0; x < call->width; x++) {
            uint32_t got = pixel_at(stacked, x, y);
            uint32_t s = pixel_at(picture, x, y) >> 24;
            uint32_t want = s == 0 ? below : premultiplied_over(below, rgb, s);
            expect(got == want, call->name, "stacked draw", x, y, got, want);
            expect(channels_within_alpha(got), call->name, "channels within alpha", x, y, got, want);
        }
    }

    stlxgfx_destroy_surface(stacked);
}

/* Onto opaque pixels the flag changes nothing */
static void check_opaque_draws(const draw_call* call, uint32_t rgb) {
    uint32_t opaque = OPAQUE | BELOW_COLOR;
    stlxgfx_surface_t* flagged = new_surface(call->width, call->height, STLXGFX_SURFACE_PREMULTIPLIED, opaque);
    stlxgfx_surface_t* plain = new_surface(call->width, call->height, 0, opaque);

    call->draw(flagged, rgb);
    call->draw(plain, rgb);

    for (uint32_t y = 0; y < call->height; y++) {
        for (uint32_t x = 0; x < call->width; x++) {
            uint32_t got = pixel_at(flagged, x, y);
            uint32_t want = pixel_at(plain, x, y);
            expect(got == want, call->name, "opaque pixel as without the flag", x, y, got, want);
        }
    }

    stlxgfx_destroy_surface(flagged);
    stlxgfx_destroy_surface(plain);
}

/* Every check of `call` drawing `rgb`: onto transparent pixels, over each alpha of two colors and over opaque ones */
static void check_call(const draw_call* call, uint32_t rgb) {
    stlxgfx_surface_t* picture = new_surface(call->width, call->height, STLXGFX_SURFACE_PREMULTIPLIED, 0);

    call->draw(picture, rgb);
    check_single_draws(call, picture, rgb);

    for (uint32_t d = 0; d < CHANNEL_VALUES; d++) {
        check_stacked_draws(call, picture, rgb, (d << 24) | WHITE);
        check_stacked_draws(call, picture, rgb, (d << 24) | BELOW_COLOR);
    }

    check_opaque_draws(call, rgb);
    stlxgfx_destroy_surface(picture);
}

int main(void) {
    static const char font_path[] = INITRD_DIR STLXTHEME_MONO_FONT_PATH;
    static const draw_call calls[] = {
        { "blend_coverage",             SWEEP_WIDTH, SWEEP_HEIGHT, draw_blend_coverage        },
        { "blend_coverage translucent", SWEEP_WIDTH, SWEEP_HEIGHT, draw_translucent_coverage  },
        { "fill_rect_blend",            SWEEP_WIDTH, SWEEP_HEIGHT, draw_fill_rect_blend       },
        { "blit_alpha",                 SWEEP_WIDTH, SWEEP_HEIGHT, draw_blit_alpha            },
        { "blit_rounded_alpha",         SHAPE_SIZE,  SHAPE_SIZE,   draw_blit_rounded_alpha    },
        { "blit_arc_corner",            SHAPE_SIZE,  SHAPE_SIZE,   draw_blit_arc_corner       },
        { "draw_text",                  TEXT_WIDTH,  TEXT_HEIGHT,  draw_text                  },
        { "draw_text translucent",      TEXT_WIDTH,  TEXT_HEIGHT,  draw_translucent_text      },
    };

    /* White and black reach both ends of every channel, the others mix them */
    static const uint32_t colors[] = { 0xFFFFFFu, 0x000000u, 0xFF8020u, 0x2080FFu };

    g_font = stlxgfx_font_open(font_path, FONT_PX);
    if (!g_font) {
        fprintf(stderr, "premultiplied: cannot open %s\n", font_path);
        return 1;
    }

    check_fills();
    check_lines();
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        for (size_t j = 0; j < sizeof(colors) / sizeof(colors[0]); j++) {
            check_call(&calls[i], colors[j]);
        }
    }

    stlxgfx_font_close(g_font);

    if (g_failures != 0) {
        printf(
            "premultiplied: %llu of %llu checks failed\n",
            (unsigned long long)g_failures,
            (unsigned long long)g_checks
        );
        return 1;
    }

    printf("premultiplied: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
