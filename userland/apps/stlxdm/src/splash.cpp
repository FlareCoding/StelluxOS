/* The boot splash: stars dropping out of warp into a slowly breathing
 * nebula while the title resolves, until Enter dismisses it.
 */
#include "splash.hpp"
#include "presenter.hpp"

#include <stlx/input.h>
#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

constexpr uint32_t SPLASH_FPS = 60;
constexpr uint64_t SPLASH_FRAME_NS = 1000000000ull / SPLASH_FPS;
constexpr uint32_t SPLASH_STAR_COUNT = 800;
constexpr uint32_t SPLASH_TITLE_PX = 46;
constexpr uint32_t SPLASH_HINT_PX = 15;

/* Text breathes between a dim and a bright shade of starlight */
constexpr uint32_t SPLASH_TEXT_BRIGHT = 0xFFE9EDF8;
constexpr uint32_t SPLASH_TEXT_MID = 0xFFB7C0D8;
constexpr uint32_t SPLASH_TEXT_DIM = 0xFF7F8AA6;

/* The warp-in: stars start fast and settle to cruise while the title
 * fades up, then the hint follows */
constexpr float SPLASH_WARP_SPEED = 0.05f;
constexpr float SPLASH_CRUISE_SPEED = 0.004f;
constexpr float SPLASH_WARP_S = 1.8f;
constexpr float SPLASH_TITLE_IN_S = 0.4f;
constexpr float SPLASH_TITLE_FADE_S = 1.3f;
constexpr float SPLASH_HINT_IN_S = 1.7f;
constexpr float SPLASH_HINT_FADE_S = 0.8f;

/* The nebula's two lobes, violet and a fainter ion cyan */
constexpr float SPLASH_NEBULA_R = 26.0f;
constexpr float SPLASH_NEBULA_G = 18.0f;
constexpr float SPLASH_NEBULA_B = 52.0f;
constexpr float SPLASH_ION_R = 6.0f;
constexpr float SPLASH_ION_G = 26.0f;
constexpr float SPLASH_ION_B = 34.0f;

/* The nebula is evaluated per block of this many pixels, in fixed
 * point with this many fraction bits. Every float op is a helper
 * call under emulation, so the per block math is integer only. */
constexpr int32_t NEBULA_BLOCK = 4;
constexpr int32_t NEBULA_Q = 14;
constexpr int32_t NEBULA_ONE = 1 << NEBULA_Q;

struct splash_star {
    float x, y, z;
    uint8_t tint;
};

/* The nebula field is a sum of products of a function of x and a
 * function of y, so a frame rebuilds one short table per axis and the
 * block loop only multiplies. The radial falloff is the one term that
 * does not separate, and it never changes, so it is tabulated once. */
struct nebula_field {
    int32_t cols = 0;
    int32_t rows = 0;
    std::vector<int16_t> falloff;   /* rows x cols */
    std::vector<int32_t> sx, s2x, c2x, lx;
    std::vector<int32_t> cy, c2y, s2y, ly;
    std::vector<uint32_t> row_colors;
};

static splash_star g_stars[SPLASH_STAR_COUNT];
static uint32_t g_rng_state = 0xDEADBEEF;
static nebula_field g_nebula;

static uint32_t splash_rand() {
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 17;
    g_rng_state ^= g_rng_state << 5;

    return g_rng_state;
}

static float splash_randf() {
    return static_cast<float>(splash_rand() & 0xFFFF) / 65535.0f;
}

static void splash_init_star(splash_star& s, bool full_depth) {
    s.x = (splash_randf() - 0.5f) * 2.0f;
    s.y = (splash_randf() - 0.5f) * 2.0f;
    s.z = full_depth ? splash_randf() : (0.001f + splash_randf() * 0.05f);
    s.tint = static_cast<uint8_t>(splash_rand() % 4);
}

static void splash_update_stars(float speed) {
    for (uint32_t i = 0; i < SPLASH_STAR_COUNT; i++) {
        g_stars[i].z -= speed;
        if (g_stars[i].z <= 0.001f) {
            splash_init_star(g_stars[i], false);
            g_stars[i].z = 0.9f + splash_randf() * 0.1f;
        }
    }
}

static int32_t nebula_q(float v) {
    return static_cast<int32_t>(lrintf(v * static_cast<float>(NEBULA_ONE)));
}

/* Sizes the tables for the screen and fills the static falloff */
static void nebula_init(uint32_t w, uint32_t h) {
    nebula_field& n = g_nebula;
    n.cols = (static_cast<int32_t>(w) + NEBULA_BLOCK - 1) / NEBULA_BLOCK;
    n.rows = (static_cast<int32_t>(h) + NEBULA_BLOCK - 1) / NEBULA_BLOCK;

    n.falloff.assign(static_cast<size_t>(n.cols) * static_cast<size_t>(n.rows), 0);
    for (auto* v : { &n.sx, &n.s2x, &n.c2x, &n.lx }) {
        v->assign(static_cast<size_t>(n.cols), 0);
    }
    for (auto* v : { &n.cy, &n.c2y, &n.s2y, &n.ly }) {
        v->assign(static_cast<size_t>(n.rows), 0);
    }
    n.row_colors.assign(static_cast<size_t>(n.cols), 0);

    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) * 0.5f;
    for (int32_t by = 0; by < n.rows; by++) {
        float dy = (static_cast<float>(by * NEBULA_BLOCK) - cy) / cy;
        for (int32_t bx = 0; bx < n.cols; bx++) {
            float dx = (static_cast<float>(bx * NEBULA_BLOCK) - cx) / cx;
            float falloff = 1.0f - sqrtf(dx * dx + dy * dy) * 0.7f;
            if (falloff < 0.0f) {
                falloff = 0.0f;
            }

            n.falloff[static_cast<size_t>(by) * static_cast<size_t>(n.cols)
                      + static_cast<size_t>(bx)] = static_cast<int16_t>(nebula_q(falloff));
        }
    }
}

/* Two lobes of drifting light: the violet body is sin(a + t) times
 * cos(b - 0.7t) plus sin(a' + b' + 0.5t), the cyan cast is one more
 * product, and every term factors by axis. The per frame tables hold
 * the axis factors, and each block multiplies them. */
static void splash_draw_nebula(stlxgfx_surface_t* buf, uint32_t w,
                               uint32_t h, uint32_t frame) {
    nebula_field& n = g_nebula;
    float t = static_cast<float>(frame) * 0.003f;
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) * 0.5f;

    for (int32_t bx = 0; bx < n.cols; bx++) {
        float dx = (static_cast<float>(bx * NEBULA_BLOCK) - cx) / cx;
        size_t i = static_cast<size_t>(bx);
        n.sx[i] = nebula_q(sinf(dx * 2.5f + t));
        n.s2x[i] = nebula_q(sinf(dx * 1.8f + t * 0.5f));
        n.c2x[i] = nebula_q(cosf(dx * 1.8f + t * 0.5f));
        n.lx[i] = nebula_q(sinf(dx * 1.6f - t * 0.6f + 1.2f));
    }
    for (int32_t by = 0; by < n.rows; by++) {
        float dy = (static_cast<float>(by * NEBULA_BLOCK) - cy) / cy;
        size_t i = static_cast<size_t>(by);
        n.cy[i] = nebula_q(cosf(dy * 3.0f - t * 0.7f));
        n.c2y[i] = nebula_q(cosf(dy * 1.8f));
        n.s2y[i] = nebula_q(sinf(dy * 1.8f));
        n.ly[i] = nebula_q(cosf(dy * 2.2f + t * 0.4f));
    }

    /* Channel weights as integers, applied to Q values and shifted
     * back, matching the float truncation to within one step */
    const int32_t body_r = static_cast<int32_t>(SPLASH_NEBULA_R);
    const int32_t body_g = static_cast<int32_t>(SPLASH_NEBULA_G);
    const int32_t body_b = static_cast<int32_t>(SPLASH_NEBULA_B);
    const int32_t ion_r = static_cast<int32_t>(SPLASH_ION_R);
    const int32_t ion_g = static_cast<int32_t>(SPLASH_ION_G);
    const int32_t ion_b = static_cast<int32_t>(SPLASH_ION_B);

    for (int32_t by = 0; by < n.rows; by++) {
        const int16_t* fall = n.falloff.data() + static_cast<size_t>(by) * static_cast<size_t>(n.cols);
        int32_t cyv = n.cy[static_cast<size_t>(by)];
        int32_t c2yv = n.c2y[static_cast<size_t>(by)];
        int32_t s2yv = n.s2y[static_cast<size_t>(by)];
        int32_t lyv = n.ly[static_cast<size_t>(by)];

        for (int32_t bx = 0; bx < n.cols; bx++) {
            size_t i = static_cast<size_t>(bx);
            int32_t f = fall[i];

            int32_t n1 = (n.sx[i] * cyv) >> NEBULA_Q;
            int32_t n2 = (n.s2x[i] * c2yv + n.c2x[i] * s2yv) >> NEBULA_Q;
            int32_t v = (((n1 + n2) >> 1) * f) >> NEBULA_Q;
            if (v < 0) {
                v = 0;
            }

            int32_t c = ((n.lx[i] * lyv) >> NEBULA_Q) * f >> NEBULA_Q;
            if (c < 0) {
                c = 0;
            }
            c >>= 1;

            uint32_t r = static_cast<uint32_t>((v * body_r + c * ion_r) >> NEBULA_Q);
            uint32_t g = static_cast<uint32_t>((v * body_g + c * ion_g) >> NEBULA_Q);
            uint32_t b = static_cast<uint32_t>((v * body_b + c * ion_b) >> NEBULA_Q);
            n.row_colors[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }

        /* The block row's first scanline is filled once and copied to
         * the rows below it. Every pixel is covered, so no clear runs. */
        int32_t y0 = by * NEBULA_BLOCK;
        int32_t y1 = y0 + NEBULA_BLOCK < static_cast<int32_t>(h) ? y0 + NEBULA_BLOCK
                                                                  : static_cast<int32_t>(h);
        uint32_t* first = reinterpret_cast<uint32_t*>(
            buf->pixels + static_cast<uint32_t>(y0) * buf->pitch);
        int32_t whole = static_cast<int32_t>(w) / NEBULA_BLOCK;
        uint32_t* out = first;
        for (int32_t bx = 0; bx < whole; bx++) {
            uint32_t color = n.row_colors[static_cast<size_t>(bx)];
            out[0] = color;
            out[1] = color;
            out[2] = color;
            out[3] = color;
            out += NEBULA_BLOCK;
        }
        for (int32_t x = whole * NEBULA_BLOCK; x < static_cast<int32_t>(w); x++) {
            first[x] = n.row_colors[static_cast<size_t>(whole)];
        }

        for (int32_t y = y0 + 1; y < y1; y++) {
            memcpy(buf->pixels + static_cast<uint32_t>(y) * buf->pitch, first,
                   static_cast<size_t>(w) * 4);
        }
    }
}

static void splash_draw_stars(stlxgfx_surface_t* buf, uint32_t w,
                              uint32_t h) {
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) * 0.5f;

    for (uint32_t i = 0; i < SPLASH_STAR_COUNT; i++) {
        splash_star& s = g_stars[i];
        float inv_z = 1.0f / s.z;
        int32_t sx = static_cast<int32_t>(cx + s.x * inv_z * cx);
        int32_t sy = static_cast<int32_t>(cy + s.y * inv_z * cy);
        if (sx < 0 || sy < 0 || sx >= static_cast<int32_t>(w) ||
            sy >= static_cast<int32_t>(h)) {
            continue;
        }

        float brightness = 1.0f - s.z;
        if (brightness < 0.0f) brightness = 0.0f;
        if (brightness > 1.0f) brightness = 1.0f;
        brightness = brightness * brightness;

        uint8_t r, g, b;
        switch (s.tint) {
        case 0:
            r = static_cast<uint8_t>(brightness * 255.0f);
            g = static_cast<uint8_t>(brightness * 240.0f);
            b = static_cast<uint8_t>(brightness * 255.0f);
            break;
        case 1:
            r = static_cast<uint8_t>(brightness * 200.0f);
            g = static_cast<uint8_t>(brightness * 220.0f);
            b = static_cast<uint8_t>(brightness * 255.0f);
            break;
        case 2:
            r = static_cast<uint8_t>(brightness * 255.0f);
            g = static_cast<uint8_t>(brightness * 200.0f);
            b = static_cast<uint8_t>(brightness * 180.0f);
            break;
        default:
            r = static_cast<uint8_t>(brightness * 255.0f);
            g = static_cast<uint8_t>(brightness * 255.0f);
            b = static_cast<uint8_t>(brightness * 255.0f);
            break;
        }

        uint32_t color = 0xFF000000 | (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(g) << 8) |
                         static_cast<uint32_t>(b);
        int32_t size = 1;
        if (brightness > 0.6f) {
            size = 2;
        }
        if (brightness > 0.85f) {
            size = 3;
        }
        stlxgfx_fill_rect(buf, sx, sy,
                          static_cast<uint32_t>(size),
                          static_cast<uint32_t>(size), color);

        if (brightness > 0.92f) {
            uint32_t glow = 0xFF000000 |
                            (static_cast<uint32_t>(r / 4) << 16) |
                            (static_cast<uint32_t>(g / 4) << 8) |
                            static_cast<uint32_t>(b / 4);
            stlxgfx_fill_rect(buf, sx - 1, sy, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx + size, sy, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx, sy - 1, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx, sy + size, 1, 1, glow);
        }
    }
}

static void splash_draw_centered(stlxgfx_surface_t* buf, uint32_t screen_w,
                                 int32_t top_y, const char* text,
                                 stlxgfx_font* font,
                                 const stlxgfx_font_metrics& fm,
                                 uint32_t color) {
    int32_t tw = stlxgfx_text_width(font, text, strlen(text));
    int32_t tx = (static_cast<int32_t>(screen_w) - tw) / 2;

    stlxgfx_draw_text(buf, font, tx, top_y + fm.ascent, text,
                      strlen(text), color);
}

/* Blends two palette colors by t in [0, 1] */
static uint32_t splash_mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        float ca = static_cast<float>((a >> shift) & 0xFF);
        float cb = static_cast<float>((b >> shift) & 0xFF);
        uint32_t v = static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f);
        out |= (v & 0xFF) << shift;
    }

    return out;
}

/* A slow breath between two palette shades, scaled by a fade alpha */
static uint32_t splash_pulse_color(float seconds, uint32_t lo, uint32_t hi,
                                   float alpha) {
    float pulse = 0.5f + 0.5f * sinf(seconds * 2.0f * 3.14159f * 0.4f);
    uint32_t color = splash_mix(lo, hi, pulse);
    uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);

    return (a << 24) | (color & 0x00FFFFFFu);
}

/* Linear ramp from 0 at start to 1 after duration */
static float splash_ramp(float seconds, float start, float duration) {
    float t = (seconds - start) / duration;
    if (t < 0.0f) {
        return 0.0f;
    }
    if (t > 1.0f) {
        return 1.0f;
    }

    return t * t * (3.0f - 2.0f * t);
}

static bool splash_check_enter(int kbd_fd) {
    if (kbd_fd < 0) {
        return false;
    }

    stlx_input_kbd_event_t buf[16];
    ssize_t n = read(kbd_fd, buf, sizeof(buf));
    if (n <= 0) {
        return false;
    }

    int count = static_cast<int>(
        n / static_cast<ssize_t>(sizeof(stlx_input_kbd_event_t)));
    for (int i = 0; i < count; i++) {
        if (buf[i].action == STLX_INPUT_KBD_ACTION_DOWN &&
            buf[i].usage == 0x28) {
            return true;
        }
    }

    return false;
}

static uint64_t splash_clock_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}

void splash_run(presenter& pres) {
    stlxgfx_font* title_font = stlxgfx_font_open(STLXGFX_UI_FONT_SEMIBOLD_PATH,
                                                 SPLASH_TITLE_PX);
    stlxgfx_font* hint_font = stlxgfx_font_open(STLXGFX_UI_FONT_PATH,
                                                SPLASH_HINT_PX);
    if (!title_font || !hint_font) {
        stlxgfx_font_close(title_font);
        stlxgfx_font_close(hint_font);
        return;
    }

    stlxgfx_font_metrics title_fm, hint_fm;
    stlxgfx_font_metrics_get(title_font, &title_fm);
    stlxgfx_font_metrics_get(hint_font, &hint_fm);

    int kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);

    for (uint32_t i = 0; i < SPLASH_STAR_COUNT; i++) {
        splash_init_star(g_stars[i], true);
    }

    uint32_t w = pres.width();
    uint32_t h = pres.height();
    int32_t title_y = static_cast<int32_t>(h / 2) - 40;
    int32_t hint_y = static_cast<int32_t>(h / 2) + 34;
    nebula_init(w, h);

    damage_list full;
    full.add_full();

    uint64_t start_ns = splash_clock_ns();
    uint32_t frame = 0;
    while (true) {
        uint64_t frame_start = splash_clock_ns();
        float seconds = static_cast<float>(frame_start - start_ns) / 1e9f;

        if (splash_check_enter(kbd_fd)) {
            break;
        }

        presenter::target t = pres.acquire();
        stlxgfx_surface_t* buf = stlxgfx_surface_from_buffer(
            reinterpret_cast<uint8_t*>(t.pixels), w, h, t.stride,
            32, 16, 8, 0);
        if (!buf) {
            break;
        }

        /* Warp decays into cruise as the title resolves */
        float settle = splash_ramp(seconds, 0.0f, SPLASH_WARP_S);
        float speed = SPLASH_WARP_SPEED
                    + (SPLASH_CRUISE_SPEED - SPLASH_WARP_SPEED) * settle;
        splash_update_stars(speed);

        splash_draw_nebula(buf, w, h, frame);
        splash_draw_stars(buf, w, h);

        float title_a = splash_ramp(seconds, SPLASH_TITLE_IN_S, SPLASH_TITLE_FADE_S);
        float hint_a = splash_ramp(seconds, SPLASH_HINT_IN_S, SPLASH_HINT_FADE_S);
        splash_draw_centered(buf, w, title_y, "Stellux 3.0", title_font,
                             title_fm,
                             splash_pulse_color(seconds, SPLASH_TEXT_MID,
                                                SPLASH_TEXT_BRIGHT, title_a));
        splash_draw_centered(buf, w, hint_y, "Press Enter to continue",
                             hint_font, hint_fm,
                             splash_pulse_color(seconds + 0.6f, SPLASH_TEXT_DIM,
                                                SPLASH_TEXT_MID, hint_a));

        pres.present(full);
        stlxgfx_destroy_surface(buf);
        frame++;

        uint64_t elapsed = splash_clock_ns() - frame_start;
        if (elapsed < SPLASH_FRAME_NS) {
            timespec rem = {
                0, static_cast<long>(SPLASH_FRAME_NS - elapsed)
            };
            nanosleep(&rem, nullptr);
        }
    }

    if (kbd_fd >= 0) {
        close(kbd_fd);
    }

    stlxgfx_font_close(title_font);
    stlxgfx_font_close(hint_font);
}
