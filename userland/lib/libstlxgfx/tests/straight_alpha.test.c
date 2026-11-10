/* Each blending call onto straight alpha surfaces, run on the build machine over transparent,
 * partly covered and opaque pixels and checked against the rule restated here */

#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxtheme/theme.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWEEP_WIDTH  256
#define SWEEP_HEIGHT 3
#define SHAPE_SIZE   64
#define BAND_HEIGHT  16
#define TEXT_WIDTH   160
#define TEXT_HEIGHT  32
#define FONT_PX      20
#define MAX_REPORTS  8
#define OPAQUE       0xFF000000u
#define TRANSLUCENT  0x80000000u

/* One blending call, drawing color `rgb` at the alpha each pixel gets from the call's geometry or
 * source, each pixel at most once, since two draws stacked and blended once round differently */
typedef struct {
    const char* name;
    uint32_t    width;
    uint32_t    height;
    void      (*draw)(stlxgfx_surface_t* dst, uint32_t rgb);
    int         alpha_is_column;
} draw_call;

typedef struct {
    uint32_t drawn;
    uint32_t below;
} color_pair;

static stlxgfx_font* g_font;
static uint64_t      g_checks;
static uint64_t      g_failures;

static stlxgfx_surface_t* new_surface(
    uint32_t width,
    uint32_t height,
    uint32_t flags,
    uint32_t argb
) {
    stlxgfx_surface_t* s = stlxgfx_create_surface(width, height, 32, 16, 8, 0);
    if (!s) {
        fprintf(stderr, "straight alpha: out of memory\n");
        exit(1);
    }

    s->flags = flags;
    stlxgfx_clear(s, argb);
    return s;
}

/* A source whose alpha varies along both rows and columns */
static stlxgfx_surface_t* new_gradient(uint32_t width, uint32_t height, uint32_t rgb) {
    stlxgfx_surface_t* src = new_surface(width, height, 0, 0);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t alpha = (x * 4 + y * 3) & 0xFF;
            stlxgfx_fill_rect(src, (int32_t)x, (int32_t)y, 1, 1, (alpha << 24) | rgb);
        }
    }

    return src;
}

static uint32_t pixel_at(const stlxgfx_surface_t* s, uint32_t x, uint32_t y) {
    const uint8_t* p = s->pixels + y * s->pitch + x * 4;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void sweep_blend_coverage(stlxgfx_surface_t* dst, uint32_t argb) {
    for (uint32_t y = 0; y < SWEEP_HEIGHT; y++) {
        for (uint32_t x = 0; x < SWEEP_WIDTH; x++) {
            stlxgfx_blend_coverage(dst, (int32_t)x, (int32_t)y, argb, (uint8_t)x);
        }
    }
}

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

/* Bands past the surface's sides, the first also above it, so each fill blends whole rows from
 * clipped bounds */
static void draw_fill_rect_blend_bands(stlxgfx_surface_t* dst, uint32_t rgb) {
    static const uint32_t alphas[] = { 1, 128, 254, 255 };

    for (uint32_t i = 0; i < sizeof(alphas) / sizeof(alphas[0]); i++) {
        int32_t top = i == 0 ? -4 : (int32_t)(i * BAND_HEIGHT);
        int32_t bottom = (int32_t)((i + 1) * BAND_HEIGHT);
        uint32_t argb = (alphas[i] << 24) | rgb;
        stlxgfx_fill_rect_blend(dst, -3, top, SHAPE_SIZE + 6, (uint32_t)(bottom - top), argb);
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

/* Part of a wider source, from an offset onto an origin the surface clips */
static void draw_blit_alpha_offset(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_gradient(SHAPE_SIZE + 16, SHAPE_SIZE, rgb);
    stlxgfx_blit_alpha(dst, -2, 4, src, 5, 3, SHAPE_SIZE, SHAPE_SIZE - 8);
    stlxgfx_destroy_surface(src);
}

static void draw_blit_rounded_alpha(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_gradient(SHAPE_SIZE, SHAPE_SIZE, rgb);
    stlxgfx_blit_rounded_alpha(dst, 0, 0, src, 0, 0, SHAPE_SIZE, SHAPE_SIZE, 20);
    stlxgfx_destroy_surface(src);
}

static void draw_blit_arc_corner(stlxgfx_surface_t* dst, uint32_t rgb) {
    stlxgfx_surface_t* src = new_surface(SHAPE_SIZE, SHAPE_SIZE, 0, OPAQUE | rgb);
    stlxgfx_blit_arc_corner(dst, 0, 0, 48, 20, 1, 1, 0, src);
    stlxgfx_destroy_surface(src);
}

static void write_text(stlxgfx_surface_t* dst, uint32_t argb) {
    static const char text[] = "Stellux 0.1";
    stlxgfx_draw_text(dst, g_font, 2, 24, text, strlen(text), argb);
}

static void draw_text(stlxgfx_surface_t* dst, uint32_t rgb) {
    write_text(dst, OPAQUE | rgb);
}

static void draw_translucent_text(stlxgfx_surface_t* dst, uint32_t rgb) {
    write_text(dst, TRANSLUCENT | rgb);
}

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
        printf("  FAIL: %s, %s at (%u, %u): 0x%08X, expected 0x%08X\n",
               call, what, x, y, got, want);
    }
}

/* `c` drawn at alpha `s` onto alpha `d` below 255 stores the coverage of both together, rounded up,
 * and each channel the mean of both colors weighted by the coverage each shows, rounded */
static uint32_t straight_over(uint32_t below, uint32_t c, uint32_t s) {
    uint32_t d = below >> 24;
    uint32_t w = s * 255 + d * (255 - s);
    uint32_t a = s + (d * (255 - s) + 254) / 255;

    uint32_t over = a << 24;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t drawn = (c >> shift) & 0xFF;
        uint32_t kept = (below >> shift) & 0xFF;
        over |= ((drawn * s * 255 + kept * d * (255 - s) + w / 2) / w) << shift;
    }

    return over;
}

static int channels_between(uint32_t got, uint32_t c, uint32_t e) {
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t channel = (got >> shift) & 0xFF;
        uint32_t drawn = (c >> shift) & 0xFF;
        uint32_t kept = (e >> shift) & 0xFF;
        uint32_t low = drawn < kept ? drawn : kept;
        uint32_t high = drawn < kept ? kept : drawn;
        if (channel < low || channel > high) {
            return 0;
        }
    }

    return 1;
}

/* Onto transparent pixels each drawn pixel stores the call's color, at its column's alpha when
 * `alpha_is_column`, and the rest stay as they were */
static void check_single_draws(
    const draw_call* call,
    const stlxgfx_surface_t* picture,
    color_pair colors
) {
    uint32_t partial = 0;

    for (uint32_t y = 0; y < call->height; y++) {
        for (uint32_t x = 0; x < call->width; x++) {
            uint32_t got = pixel_at(picture, x, y);
            uint32_t s = got >> 24;
            uint32_t want = s == 0 ? colors.below : (s << 24) | colors.drawn;
            expect(got == want, call->name, "single draw", x, y, got, want);

            if (call->alpha_is_column) {
                expect(s == x, call->name, "stored alpha", x, y, got, (x << 24) | colors.drawn);
            }

            partial += s != 0 && s != 255;
        }
    }

    g_checks++;
    if (partial == 0) {
        g_failures++;
        printf("  FAIL: %s, no partly covered pixel drawn\n", call->name);
    }
}

/* Blending the picture once onto an opaque surface gives what the same
 * call draws directly onto that surface */
static void check_blended_once(
    const draw_call* call,
    const stlxgfx_surface_t* picture,
    color_pair colors
) {
    uint32_t backgrounds[] = { OPAQUE, 0xFFFFFFFFu, OPAQUE | colors.below };

    for (size_t i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]); i++) {
        stlxgfx_surface_t* drawn = new_surface(call->width, call->height, 0, backgrounds[i]);
        stlxgfx_surface_t* blended = new_surface(call->width, call->height, 0, backgrounds[i]);

        call->draw(drawn, colors.drawn);
        stlxgfx_blit_alpha(blended, 0, 0, picture, 0, 0, call->width, call->height);

        for (uint32_t y = 0; y < call->height; y++) {
            for (uint32_t x = 0; x < call->width; x++) {
                uint32_t got = pixel_at(blended, x, y);
                uint32_t want = pixel_at(drawn, x, y);
                expect(got == want, call->name, "picture blended once", x, y, got, want);
            }
        }

        stlxgfx_destroy_surface(drawn);
        stlxgfx_destroy_surface(blended);
    }
}

/* Over alpha 1 to 254 a draw follows the rule, stores no less alpha than both draws cover and keeps
 * each channel between both colors, so a color drawn over itself stays */
static void check_stacked_draws(
    const draw_call* call,
    const stlxgfx_surface_t* picture,
    uint32_t c,
    uint32_t below
) {
    stlxgfx_surface_t* stacked = new_surface(
        call->width,
        call->height,
        STLXGFX_SURFACE_STRAIGHT_ALPHA,
        below
    );
    uint32_t d = below >> 24;

    call->draw(stacked, c);

    for (uint32_t y = 0; y < call->height; y++) {
        for (uint32_t x = 0; x < call->width; x++) {
            uint32_t got = pixel_at(stacked, x, y);
            uint32_t s = pixel_at(picture, x, y) >> 24;
            if (s == 0) {
                expect(got == below, call->name, "pixel left as it was", x, y, got, below);
                continue;
            }

            uint32_t want = straight_over(below, c, s);
            uint32_t a = got >> 24;
            int is_between = channels_between(got, c, below);
            int covers_both = a * 255 >= s * 255 + d * (255 - s);

            expect(got == want, call->name, "stacked draw", x, y, got, want);
            expect(is_between, call->name, "channels between both colors", x, y, got, want);
            expect(covers_both, call->name, "alpha covering both draws", x, y, got, want);
        }
    }

    stlxgfx_destroy_surface(stacked);
}

/* Onto opaque pixels the flag changes nothing */
static void check_opaque_draws(const draw_call* call, color_pair colors) {
    uint32_t opaque = OPAQUE | colors.below;
    stlxgfx_surface_t* flagged = new_surface(
        call->width,
        call->height,
        STLXGFX_SURFACE_STRAIGHT_ALPHA,
        opaque
    );
    stlxgfx_surface_t* plain = new_surface(call->width, call->height, 0, opaque);

    call->draw(flagged, colors.drawn);
    call->draw(plain, colors.drawn);

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

static void check_call(const draw_call* call, color_pair colors) {
    stlxgfx_surface_t* picture = new_surface(
        call->width,
        call->height,
        STLXGFX_SURFACE_STRAIGHT_ALPHA,
        colors.below
    );

    call->draw(picture, colors.drawn);
    check_single_draws(call, picture, colors);
    check_blended_once(call, picture, colors);

    for (uint32_t d = 1; d < 255; d++) {
        check_stacked_draws(call, picture, colors.drawn, (d << 24) | colors.below);
    }

    check_opaque_draws(call, colors);
    stlxgfx_destroy_surface(picture);
}

int main(void) {
    static const char font_path[] = INITRD_DIR STLXTHEME_MONO_FONT_PATH;
    static const draw_call calls[] = {
        { "blend_coverage",             SWEEP_WIDTH, SWEEP_HEIGHT, draw_blend_coverage,        1 },
        { "blend_coverage translucent", SWEEP_WIDTH, SWEEP_HEIGHT, draw_translucent_coverage,  0 },
        { "fill_rect_blend",            SWEEP_WIDTH, SWEEP_HEIGHT, draw_fill_rect_blend,       1 },
        { "fill_rect_blend bands",      SHAPE_SIZE,  SHAPE_SIZE,   draw_fill_rect_blend_bands, 0 },
        { "blit_alpha",                 SWEEP_WIDTH, SWEEP_HEIGHT, draw_blit_alpha,            1 },
        { "blit_alpha offset",          SHAPE_SIZE,  SHAPE_SIZE,   draw_blit_alpha_offset,     0 },
        { "blit_rounded_alpha",         SHAPE_SIZE,  SHAPE_SIZE,   draw_blit_rounded_alpha,    0 },
        { "blit_arc_corner",            SHAPE_SIZE,  SHAPE_SIZE,   draw_blit_arc_corner,       0 },
        { "draw_text",                  TEXT_WIDTH,  TEXT_HEIGHT,  draw_text,                  0 },
        { "draw_text translucent",      TEXT_WIDTH,  TEXT_HEIGHT,  draw_translucent_text,      0 },
    };

    /* The first pair's red is drawn over less red, its green over the same and its blue over more.
     * The second draws a color over itself, and the third black over white */
    static const color_pair pairs[] = {
        { 0xFF8020u, 0x2080FFu },
        { 0x2080FFu, 0x2080FFu },
        { 0x000000u, 0xFFFFFFu },
    };

    g_font = stlxgfx_font_open(font_path, FONT_PX);
    if (!g_font) {
        fprintf(stderr, "straight alpha: cannot open %s\n", font_path);
        return 1;
    }

    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        for (size_t j = 0; j < sizeof(pairs) / sizeof(pairs[0]); j++) {
            check_call(&calls[i], pairs[j]);
        }
    }

    stlxgfx_font_close(g_font);

    if (g_failures != 0) {
        printf("straight alpha: %llu of %llu checks failed\n",
               (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("straight alpha: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
