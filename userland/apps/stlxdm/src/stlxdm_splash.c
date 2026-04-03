#define _POSIX_C_SOURCE 199309L
#include "stlxdm_splash.h"
#include <stlxgfx/font.h>
#include <stlx/input.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Configuration --- */

#define SPLASH_TARGET_FPS       60
#define SPLASH_FRAME_NS         (1000000000 / SPLASH_TARGET_FPS)
#define SPLASH_BG_COLOR         0xFF050508
#define SPLASH_STAR_COUNT       800
#define SPLASH_TITLE_FONT_SIZE  36
#define SPLASH_HINT_FONT_SIZE   16

/* --- Starfield --- */

typedef struct {
    float x, y, z;
    uint8_t tint;
} splash_star_t;

static splash_star_t g_stars[SPLASH_STAR_COUNT];

static uint32_t g_rng_state = 0xDEADBEEF;

static uint32_t splash_rand(void) {
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 17;
    g_rng_state ^= g_rng_state << 5;
    return g_rng_state;
}

static float splash_randf(void) {
    return (float)(splash_rand() & 0xFFFF) / 65535.0f;
}

static void splash_init_star(splash_star_t* s, int full_depth) {
    s->x = (splash_randf() - 0.5f) * 2.0f;
    s->y = (splash_randf() - 0.5f) * 2.0f;
    s->z = full_depth ? splash_randf() : (0.001f + splash_randf() * 0.05f);
    s->tint = (uint8_t)(splash_rand() % 4);
}

static void splash_init_stars(void) {
    for (int i = 0; i < SPLASH_STAR_COUNT; i++) {
        splash_init_star(&g_stars[i], 1);
    }
}

static void splash_update_stars(float speed) {
    for (int i = 0; i < SPLASH_STAR_COUNT; i++) {
        g_stars[i].z -= speed;
        if (g_stars[i].z <= 0.001f) {
            splash_init_star(&g_stars[i], 0);
            g_stars[i].z = 0.9f + splash_randf() * 0.1f;
        }
    }
}

static void splash_draw_nebula(stlxgfx_surface_t* buf, uint32_t w, uint32_t h,
                                uint32_t frame) {
    float t = (float)frame * 0.003f;
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;

    for (int32_t py = 0; py < (int32_t)h; py += 4) {
        for (int32_t px = 0; px < (int32_t)w; px += 4) {
            float dx = ((float)px - cx) / cx;
            float dy = ((float)py - cy) / cy;
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

            uint8_t r = (uint8_t)(v * 18.0f);
            uint8_t g = (uint8_t)(v * 8.0f);
            uint8_t b = (uint8_t)(v * 30.0f);
            uint32_t color = 0xFF000000 | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | (uint32_t)b;
            stlxgfx_fill_rect(buf, px, py, 4, 4, color);
        }
    }
}

static void splash_draw_stars(stlxgfx_surface_t* buf, uint32_t w, uint32_t h) {
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;

    for (int i = 0; i < SPLASH_STAR_COUNT; i++) {
        splash_star_t* s = &g_stars[i];
        float inv_z = 1.0f / s->z;
        int32_t sx = (int32_t)(cx + s->x * inv_z * cx);
        int32_t sy = (int32_t)(cy + s->y * inv_z * cy);

        if (sx < 0 || sy < 0 || sx >= (int32_t)w || sy >= (int32_t)h) {
            continue;
        }

        float brightness = (1.0f - s->z);
        if (brightness < 0.0f) {
            brightness = 0.0f;
        }
        if (brightness > 1.0f) {
            brightness = 1.0f;
        }
        brightness = brightness * brightness;

        uint8_t r, g, b;
        switch (s->tint) {
        case 0:
            r = (uint8_t)(brightness * 255.0f);
            g = (uint8_t)(brightness * 240.0f);
            b = (uint8_t)(brightness * 255.0f);
            break;
        case 1:
            r = (uint8_t)(brightness * 200.0f);
            g = (uint8_t)(brightness * 220.0f);
            b = (uint8_t)(brightness * 255.0f);
            break;
        case 2:
            r = (uint8_t)(brightness * 255.0f);
            g = (uint8_t)(brightness * 200.0f);
            b = (uint8_t)(brightness * 180.0f);
            break;
        default:
            r = (uint8_t)(brightness * 255.0f);
            g = (uint8_t)(brightness * 255.0f);
            b = (uint8_t)(brightness * 255.0f);
            break;
        }

        uint32_t color = 0xFF000000 | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;

        int32_t size = 1;
        if (brightness > 0.6f) {
            size = 2;
        }
        if (brightness > 0.85f) {
            size = 3;
        }

        stlxgfx_fill_rect(buf, sx, sy, size, size, color);

        if (brightness > 0.92f) {
            uint32_t glow = 0xFF000000 | ((uint32_t)(r / 4) << 16) |
                            ((uint32_t)(g / 4) << 8) | (uint32_t)(b / 4);
            stlxgfx_fill_rect(buf, sx - 1, sy, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx + size, sy, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx, sy - 1, 1, 1, glow);
            stlxgfx_fill_rect(buf, sx, sy + size, 1, 1, glow);
        }
    }
}

/* --- Text rendering --- */

static void splash_draw_centered(stlxgfx_surface_t* buf, uint32_t screen_w,
                                  int32_t y, const char* text,
                                  uint32_t font_size, uint32_t color) {
    uint32_t tw = 0;
    uint32_t th = 0;
    stlxgfx_text_size(text, font_size, &tw, &th);
    int32_t tx = ((int32_t)screen_w - (int32_t)tw) / 2;
    stlxgfx_draw_text(buf, tx, y, text, font_size, color);
}

static uint32_t splash_pulse_color(uint32_t frame) {
    float t = (float)frame * (2.0f * 3.14159f) / (float)SPLASH_TARGET_FPS;
    float pulse = 0.5f + 0.5f * sinf(t * 0.8f);
    uint8_t lo = 0x90;
    uint8_t hi = 0xFF;
    uint8_t val = (uint8_t)(lo + (hi - lo) * pulse);
    return 0xFF000000 | ((uint32_t)val << 16) |
           ((uint32_t)val << 8) | (uint32_t)val;
}

/* --- Input --- */

static int splash_check_enter(int kbd_fd) {
    if (kbd_fd < 0) {
        return 0;
    }
    stlx_input_kbd_event_t buf[16];
    ssize_t n = read(kbd_fd, buf, sizeof(buf));
    if (n <= 0) {
        return 0;
    }
    int count = (int)(n / (ssize_t)sizeof(stlx_input_kbd_event_t));
    for (int i = 0; i < count; i++) {
        if (buf[i].action == STLX_INPUT_KBD_ACTION_DOWN && buf[i].usage == 0x28) {
            return 1;
        }
    }
    return 0;
}

/* --- Timing --- */

static uint64_t splash_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* --- Public entry point --- */

void stlxdm_show_splash(stlxgfx_fb_t* fb, stlxgfx_surface_t* backbuf) {
    int kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);

    splash_init_stars();

    uint32_t w = fb->width;
    uint32_t h = fb->height;
    int32_t title_y = (int32_t)(h / 2) - 30;
    int32_t hint_y  = (int32_t)(h / 2) + 30;

    uint32_t frame = 0;

    while (1) {
        uint64_t frame_start = splash_clock_ns();

        if (splash_check_enter(kbd_fd)) {
            break;
        }

        splash_update_stars(0.004f);

        stlxgfx_clear(backbuf, SPLASH_BG_COLOR);
        splash_draw_nebula(backbuf, w, h, frame);
        splash_draw_stars(backbuf, w, h);

        uint32_t title_color = splash_pulse_color(frame);
        splash_draw_centered(backbuf, w, title_y, "Stellux 3.0",
                              SPLASH_TITLE_FONT_SIZE, title_color);

        uint32_t hint_color = splash_pulse_color(frame + SPLASH_TARGET_FPS / 4);
        splash_draw_centered(backbuf, w, hint_y, "Press Enter to continue",
                              SPLASH_HINT_FONT_SIZE, hint_color);

        stlxgfx_fb_present(fb, backbuf);
        frame++;

        uint64_t frame_end = splash_clock_ns();
        uint64_t elapsed = frame_end - frame_start;
        if (elapsed < SPLASH_FRAME_NS) {
            struct timespec rem = {
                .tv_sec = 0,
                .tv_nsec = (long)(SPLASH_FRAME_NS - elapsed)
            };
            nanosleep(&rem, NULL);
        }
    }

    if (kbd_fd >= 0) {
        close(kbd_fd);
    }
}
