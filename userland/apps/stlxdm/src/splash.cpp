/* The boot splash: stars streaming out of a slowly breathing nebula
 * under a pulsing title, until Enter dismisses it.
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

constexpr uint32_t SPLASH_FPS = 60;
constexpr uint64_t SPLASH_FRAME_NS = 1000000000ull / SPLASH_FPS;
constexpr uint32_t SPLASH_BG_COLOR = 0xFF050508;
constexpr uint32_t SPLASH_STAR_COUNT = 800;
constexpr uint32_t SPLASH_TITLE_PX = 36;
constexpr uint32_t SPLASH_HINT_PX = 16;

struct splash_star {
    float x, y, z;
    uint8_t tint;
};

static splash_star g_stars[SPLASH_STAR_COUNT];
static uint32_t g_rng_state = 0xDEADBEEF;

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

/* Coarse 4x4 blocks keep the whole screen gradient cheap */
static void splash_draw_nebula(stlxgfx_surface_t* buf, uint32_t w,
                               uint32_t h, uint32_t frame) {
    float t = static_cast<float>(frame) * 0.003f;
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) * 0.5f;

    for (int32_t py = 0; py < static_cast<int32_t>(h); py += 4) {
        for (int32_t px = 0; px < static_cast<int32_t>(w); px += 4) {
            float dx = (static_cast<float>(px) - cx) / cx;
            float dy = (static_cast<float>(py) - cy) / cy;
            float dist = sqrtf(dx * dx + dy * dy);

            float n1 = sinf(dx * 2.5f + t) * cosf(dy * 3.0f - t * 0.7f);
            float n2 = sinf((dx + dy) * 1.8f + t * 0.5f);
            float v = (n1 + n2) * 0.5f;

            float falloff = 1.0f - dist * 0.7f;
            if (falloff < 0.0f) {
                falloff = 0.0f;
            }
            v *= falloff;
            if (v < 0.0f) {
                v = 0.0f;
            }

            uint8_t r = static_cast<uint8_t>(v * 18.0f);
            uint8_t g = static_cast<uint8_t>(v * 8.0f);
            uint8_t b = static_cast<uint8_t>(v * 30.0f);
            uint32_t color = 0xFF000000 | (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) |
                             static_cast<uint32_t>(b);
            stlxgfx_fill_rect(buf, px, py, 4, 4, color);
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

static uint32_t splash_pulse_color(uint32_t frame) {
    float t = static_cast<float>(frame) * (2.0f * 3.14159f)
            / static_cast<float>(SPLASH_FPS);
    float pulse = 0.5f + 0.5f * sinf(t * 0.8f);
    uint8_t lo = 0x90;
    uint8_t hi = 0xFF;
    uint8_t val = static_cast<uint8_t>(lo + (hi - lo) * pulse);

    return 0xFF000000 | (static_cast<uint32_t>(val) << 16) |
           (static_cast<uint32_t>(val) << 8) | static_cast<uint32_t>(val);
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
    stlxgfx_font* title_font = stlxgfx_font_open(STLXGFX_FONT_PATH,
                                                 SPLASH_TITLE_PX);
    stlxgfx_font* hint_font = stlxgfx_font_open(STLXGFX_FONT_PATH,
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
    int32_t title_y = static_cast<int32_t>(h / 2) - 30;
    int32_t hint_y = static_cast<int32_t>(h / 2) + 30;

    damage_list full;
    full.add_full();

    uint32_t frame = 0;
    while (true) {
        uint64_t frame_start = splash_clock_ns();

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

        splash_update_stars(0.004f);

        stlxgfx_clear(buf, SPLASH_BG_COLOR);
        splash_draw_nebula(buf, w, h, frame);
        splash_draw_stars(buf, w, h);
        splash_draw_centered(buf, w, title_y, "Stellux 3.0", title_font,
                             title_fm, splash_pulse_color(frame));
        splash_draw_centered(buf, w, hint_y, "Press Enter to continue",
                             hint_font, hint_fm,
                             splash_pulse_color(frame + SPLASH_FPS / 4));

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
