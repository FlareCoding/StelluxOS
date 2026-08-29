#define _POSIX_C_SOURCE 199309L
#include "stlxdm_power.h"

#include <stlxgfx/surface.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <time.h>

/* Timings. The reveal stays short so the orbs are never something the
 * user waits on, and hit testing uses the settled layout from frame one. */
#define PW_OPEN_MS      160
#define PW_CLOSE_MS     110
#define PW_HOLD_MS      700
#define PW_COMMIT_MS    560
#define PW_STAGGER_MS   40

#define PW_ORB_RADIUS   58
#define PW_ORB_GAP      216
#define PW_BLACKOUT_AT  0.82f
#define PW_STAR_MARGIN  34
#define PW_STAR_CORE    6
#define PW_STAR_GLOW    13

#define PW_DIM_COLOR        0xE60A0A12
#define PW_ORB_FILL         0xFF181826
#define PW_ORB_FILL_HOVER   0xFF20203A
#define PW_ORB_EDGE         0xFF45475A
#define PW_LABEL_DIM        0xFF9399B2
#define PW_LABEL_BRIGHT     0xFFCDD6F4
#define PW_HINT_COLOR       0xFF7F849C
#define PW_RESTART_ACCENT   0xFF89B4FA
#define PW_SHUTDOWN_ACCENT  0xFFF38BA8
#define PW_STAR_COLOR       0xFFBAC2DE
#define PW_LABEL_FONT       15
#define PW_HINT_FONT        12

static const char* const g_choice_label[2] = { "Restart", "Shut Down" };

static uint64_t pw_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Progress through the current phase, clamped to 1.0 when it has elapsed */
static float pw_phase(const stlxdm_power_t* pw, uint32_t duration_ms) {
    if (duration_ms == 0) {
        return 1.0f;
    }

    uint64_t elapsed = pw_now_ns() - pw->phase_start_ns;
    float t = (float)elapsed / ((float)duration_ms * 1000000.0f);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

static float pw_ease_out(float t) {
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static float pw_ease_in(float t) {
    return t * t;
}

static uint32_t pw_with_alpha(uint32_t color, float scale) {
    uint32_t a = (color >> 24) & 0xFFu;
    float scaled = (float)a * scale;
    if (scaled < 0.0f) scaled = 0.0f;
    if (scaled > 255.0f) scaled = 255.0f;
    return (color & 0x00FFFFFFu) | ((uint32_t)scaled << 24);
}

/* Whole screen dim. The generic blend goes through a per-pixel accessor,
 * which is far too slow at this size, so packed 32bpp surfaces get a direct
 * fixed point loop and anything else falls back. */
static void pw_dim_screen(stlxgfx_surface_t* s, uint32_t color) {
    uint32_t a = (color >> 24) & 0xFFu;
    if (a == 0) {
        return;
    }

    if (s->bpp != 32 || s->red_shift != 16 || s->green_shift != 8 ||
        s->blue_shift != 0) {
        stlxgfx_fill_rect_blend(s, 0, 0, s->width, s->height, color);
        return;
    }

    uint32_t inv = 255u - a;
    uint32_t sr = ((color >> 16) & 0xFFu) * a;
    uint32_t sg = ((color >> 8) & 0xFFu) * a;
    uint32_t sb = (color & 0xFFu) * a;

    for (uint32_t y = 0; y < s->height; y++) {
        uint32_t* row = (uint32_t*)(void*)(s->pixels + (size_t)y * s->pitch);
        for (uint32_t x = 0; x < s->width; x++) {
            uint32_t d = row[x];
            uint32_t r = ((((d >> 16) & 0xFFu) * inv) + sr) >> 8;
            uint32_t g = ((((d >> 8) & 0xFFu) * inv) + sg) >> 8;
            uint32_t b = (((d & 0xFFu) * inv) + sb) >> 8;
            row[x] = (d & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}

/* Anti-aliased filled disc. fill_circle is hard edged and opaque, and
 * these orbs sit over the desktop, so coverage blending is used instead. */
static void pw_disc(stlxgfx_surface_t* s, float cx, float cy, float radius,
                    uint32_t color) {
    if (radius <= 0.0f) {
        return;
    }

    int32_t x0 = (int32_t)floorf(cx - radius - 1.0f);
    int32_t x1 = (int32_t)ceilf(cx + radius + 1.0f);
    int32_t y0 = (int32_t)floorf(cy - radius - 1.0f);
    int32_t y1 = (int32_t)ceilf(cy + radius + 1.0f);

    /* Only the one pixel edge band needs a square root, the interior and
     * the outside are settled by comparing squared distances */
    float r_out = radius + 0.5f;
    float r_in = radius - 0.5f;
    float r_out2 = r_out * r_out;
    float r_in2 = r_in > 0.0f ? r_in * r_in : -1.0f;

    for (int32_t y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= r_out2) continue;

            if (d2 <= r_in2) {
                stlxgfx_blend_coverage(s, x, y, color, 255);
                continue;
            }

            float cov = radius + 0.5f - sqrtf(d2);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            stlxgfx_blend_coverage(s, x, y, color, (uint8_t)(cov * 255.0f));
        }
    }
}

/* Soft radial halo that fades from the core radius out to the glow radius */
static void pw_glow(stlxgfx_surface_t* s, float cx, float cy, float r_core,
                    float r_glow, uint32_t color) {
    if (r_glow <= r_core) {
        return;
    }

    int32_t x0 = (int32_t)floorf(cx - r_glow - 1.0f);
    int32_t x1 = (int32_t)ceilf(cx + r_glow + 1.0f);
    int32_t y0 = (int32_t)floorf(cy - r_glow - 1.0f);
    int32_t y1 = (int32_t)ceilf(cy + r_glow + 1.0f);

    float span = r_glow - r_core;
    float r_glow2 = r_glow * r_glow;
    float r_core2 = r_core * r_core;

    for (int32_t y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= r_glow2) continue;

            float f = 1.0f;
            if (d2 > r_core2) {
                f = (r_glow - sqrtf(d2)) / span;
            }

            f = f * f;
            stlxgfx_blend_coverage(s, x, y, color, (uint8_t)(f * 255.0f));
        }
    }
}

/* Anti-aliased arc. Angles are turns clockwise from twelve o'clock, which
 * keeps the icon and progress geometry readable in screen space. */
static void pw_arc(stlxgfx_surface_t* s, float cx, float cy, float radius,
                   float thickness, float start_turn, float sweep_turn,
                   uint32_t color) {
    if (sweep_turn <= 0.0f || thickness <= 0.0f) {
        return;
    }

    float half = thickness * 0.5f;
    float r_out = radius + half;
    float r_in = radius - half;
    if (r_in < 0.0f) r_in = 0.0f;

    int32_t x0 = (int32_t)floorf(cx - r_out - 1.0f);
    int32_t x1 = (int32_t)ceilf(cx + r_out + 1.0f);
    int32_t y0 = (int32_t)floorf(cy - r_out - 1.0f);
    int32_t y1 = (int32_t)ceilf(cy + r_out + 1.0f);

    /* The band is thin, so squared bounds reject most of the box before
     * any square root or angle work happens */
    float band_out = radius + half + 0.5f;
    float band_in = radius - half - 0.5f;
    float band_out2 = band_out * band_out;
    float band_in2 = band_in > 0.0f ? band_in * band_in : -1.0f;

    const float two_pi = 6.28318530718f;
    for (int32_t y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= band_out2 || d2 <= band_in2) continue;

            float d = sqrtf(d2);
            float cov = (half + 0.5f) - fabsf(d - radius);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float ang = atan2f(dx, -dy);
            if (ang < 0.0f) ang += two_pi;
            float turn = ang / two_pi - start_turn;
            while (turn < 0.0f) turn += 1.0f;
            if (turn > sweep_turn) continue;

            stlxgfx_blend_coverage(s, x, y, color, (uint8_t)(cov * 255.0f));
        }
    }
}

/* Filled triangle from three points, used for the restart arrow head */
static void pw_triangle(stlxgfx_surface_t* s, float ax, float ay, float bx,
                        float by, float cx, float cy, uint32_t color) {
    float min_x = fminf(ax, fminf(bx, cx));
    float max_x = fmaxf(ax, fmaxf(bx, cx));
    float min_y = fminf(ay, fminf(by, cy));
    float max_y = fmaxf(ay, fmaxf(by, cy));

    for (int32_t y = (int32_t)floorf(min_y); y <= (int32_t)ceilf(max_y); y++) {
        for (int32_t x = (int32_t)floorf(min_x); x <= (int32_t)ceilf(max_x); x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float d0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float d1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float d2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            int neg = (d0 < 0.0f) || (d1 < 0.0f) || (d2 < 0.0f);
            int pos = (d0 > 0.0f) || (d1 > 0.0f) || (d2 > 0.0f);
            if (neg && pos) continue;
            stlxgfx_blend_coverage(s, x, y, color, 255);
        }
    }
}

/* Settled centre of an orb. Hit testing always uses this, so the choices
 * are clickable from the first frame of the reveal. */
static void pw_orb_center(const stlxdm_power_t* pw, int choice,
                          float* out_cx, float* out_cy) {
    float mid_x = (float)pw->fb_width * 0.5f;
    float offset = (float)PW_ORB_GAP * 0.5f;
    *out_cx = choice == STLXDM_POWER_RESTART ? mid_x - offset : mid_x + offset;
    *out_cy = (float)pw->fb_height * 0.46f;
}

static int pw_choice_at(const stlxdm_power_t* pw, int32_t px, int32_t py) {
    for (int i = 0; i < 2; i++) {
        float cx, cy;
        pw_orb_center(pw, i, &cx, &cy);
        float dx = (float)px - cx;
        float dy = (float)py - cy;
        float r = (float)PW_ORB_RADIUS + 6.0f;
        if (dx * dx + dy * dy <= r * r) {
            return i;
        }
    }

    return -1;
}

void stlxdm_power_init(stlxdm_power_t* pw, const stlxdm_config_t* conf,
                        uint32_t fb_width, uint32_t fb_height) {
    memset(pw, 0, sizeof(*pw));
    pw->conf = conf;
    pw->fb_width = fb_width;
    pw->fb_height = fb_height;
    pw->state = STLXDM_POWER_CLOSED;
    pw->hover_choice = -1;
    pw->press_choice = -1;
    pw->commit_choice = -1;

    int32_t bar_y = (int32_t)(fb_height - conf->taskbar_height);
    pw->star_cx = (int32_t)fb_width - PW_STAR_MARGIN;
    pw->star_cy = bar_y + (int32_t)conf->taskbar_height / 2;

    /* Reserved and touched up front, so the first open pays for neither the
     * allocation nor faulting the pages of a screen sized surface in */
    pw->backdrop = stlxgfx_create_surface(fb_width, fb_height, 32, 16, 8, 0);
    if (pw->backdrop) {
        stlxgfx_clear(pw->backdrop, 0xFF000000u);
    }
}

int stlxdm_power_is_active(const stlxdm_power_t* pw) {
    return pw->state != STLXDM_POWER_CLOSED;
}

int stlxdm_power_is_animating(const stlxdm_power_t* pw) {
    return pw->state == STLXDM_POWER_OPENING ||
           pw->state == STLXDM_POWER_CLOSING ||
           pw->state == STLXDM_POWER_COMMITTING ||
           (pw->state == STLXDM_POWER_OPEN && pw->press_choice >= 0);
}

int stlxdm_power_star_hit(const stlxdm_power_t* pw, int32_t px, int32_t py) {
    int32_t dx = px - pw->star_cx;
    int32_t dy = py - pw->star_cy;
    int32_t r = PW_STAR_GLOW + 4;
    return dx * dx + dy * dy <= r * r;
}

void stlxdm_power_open(stlxdm_power_t* pw) {
    if (pw->state == STLXDM_POWER_OPENING || pw->state == STLXDM_POWER_OPEN) {
        return;
    }

    pw->state = STLXDM_POWER_OPENING;
    pw->phase_start_ns = pw_now_ns();
    pw->hover_choice = -1;
    pw->press_choice = -1;
    pw->backdrop_valid = 0;
}

/* The dim never changes once applied, so every phase except the final
 * blackout can repaint from the cache instead of re-dimming the screen. */
int stlxdm_power_is_steady(const stlxdm_power_t* pw) {
    if (!pw->backdrop_valid || pw->state == STLXDM_POWER_CLOSED) {
        return 0;
    }

    if (pw->state == STLXDM_POWER_COMMITTING) {
        return pw_phase(pw, PW_COMMIT_MS) < PW_BLACKOUT_AT;
    }

    return 1;
}

void stlxdm_power_orb_box(const stlxdm_power_t* pw, int choice,
                           int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) {
    float cx, cy;
    pw_orb_center(pw, choice, &cx, &cy);

    int32_t pad = PW_ORB_RADIUS + 44;
    *x = (int32_t)cx - pad;
    *y = (int32_t)cy - pad;
    *w = (uint32_t)(2 * pad);
    *h = (uint32_t)(pad + PW_ORB_RADIUS + 62);
}

void stlxdm_power_restore(stlxdm_power_t* pw, stlxgfx_surface_t* dst,
                           int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!pw->backdrop_valid || !pw->backdrop) {
        return;
    }

    stlxgfx_blit(dst, x, y, pw->backdrop, x, y, w, h);
}

void stlxdm_power_dismiss(stlxdm_power_t* pw) {
    if (pw->state != STLXDM_POWER_OPEN && pw->state != STLXDM_POWER_OPENING) {
        return;
    }

    /* The cache stays valid through the close so the orbs can fade out
     * cheaply, and is dropped once the desktop is composed again */
    pw->state = STLXDM_POWER_CLOSING;
    pw->phase_start_ns = pw_now_ns();
    pw->hover_choice = -1;
    pw->press_choice = -1;
}

void stlxdm_power_on_motion(stlxdm_power_t* pw, int32_t px, int32_t py) {
    if (pw->state == STLXDM_POWER_CLOSED) {
        pw->star_hover = stlxdm_power_star_hit(pw, px, py);
        return;
    }

    pw->star_hover = 0;
    if (pw->state != STLXDM_POWER_OPENING && pw->state != STLXDM_POWER_OPEN) {
        return;
    }

    pw->hover_choice = pw_choice_at(pw, px, py);

    /* Sliding off the orb abandons the hold rather than confirming blind */
    if (pw->press_choice >= 0 && pw->hover_choice != pw->press_choice) {
        pw->press_choice = -1;
    }
}

void stlxdm_power_on_press(stlxdm_power_t* pw, int32_t px, int32_t py) {
    if (pw->state != STLXDM_POWER_OPENING && pw->state != STLXDM_POWER_OPEN) {
        return;
    }

    int choice = pw_choice_at(pw, px, py);
    if (choice < 0) {
        stlxdm_power_dismiss(pw);
        return;
    }

    pw->press_choice = choice;
    pw->hold_start_ns = pw_now_ns();
}

void stlxdm_power_on_release(stlxdm_power_t* pw, int32_t px, int32_t py) {
    (void)px;
    (void)py;
    if (pw->state == STLXDM_POWER_OPENING || pw->state == STLXDM_POWER_OPEN) {
        pw->press_choice = -1;
    }
}

void stlxdm_power_update(stlxdm_power_t* pw) {
    switch (pw->state) {
    case STLXDM_POWER_OPENING:
        if (pw_phase(pw, PW_OPEN_MS) >= 1.0f) {
            pw->state = STLXDM_POWER_OPEN;
            pw->phase_start_ns = pw_now_ns();
        }
        break;
    case STLXDM_POWER_CLOSING:
        if (pw_phase(pw, PW_CLOSE_MS) >= 1.0f) {
            pw->state = STLXDM_POWER_CLOSED;
            pw->backdrop_valid = 0;
        }
        break;
    case STLXDM_POWER_COMMITTING:
        if (pw_phase(pw, PW_COMMIT_MS) >= 1.0f) {
            pw->action_ready = 1;
        }
        break;
    default:
        break;
    }

    if (pw->state != STLXDM_POWER_OPEN || pw->press_choice < 0) {
        return;
    }

    uint64_t held = pw_now_ns() - pw->hold_start_ns;
    if (held >= (uint64_t)PW_HOLD_MS * 1000000ULL) {
        pw->commit_choice = pw->press_choice;
        pw->press_choice = -1;
        pw->state = STLXDM_POWER_COMMITTING;
        pw->phase_start_ns = pw_now_ns();
    }
}

void stlxdm_power_draw_star(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx) {
    if (pw->state != STLXDM_POWER_CLOSED) {
        return;
    }

    float cx = (float)pw->star_cx;
    float cy = (float)pw->star_cy;
    float boost = pw->star_hover ? 1.0f : 0.0f;

    uint32_t halo = pw_with_alpha(PW_STAR_COLOR, 0.20f + 0.22f * boost);
    pw_glow(ctx->target, cx, cy, 1.5f, (float)PW_STAR_GLOW + 2.0f * boost, halo);
    pw_disc(ctx->target, cx, cy, (float)PW_STAR_CORE * (1.0f + 0.08f * boost),
            pw_with_alpha(PW_STAR_COLOR, 0.75f + 0.25f * boost));

    if (pw->star_hover) {
        pw_arc(ctx->target, cx, cy, (float)PW_STAR_GLOW - 1.0f, 1.5f, 0.0f,
               1.0f, pw_with_alpha(PW_STAR_COLOR, 0.55f));
    }
}

/* The power glyph: a broken ring with an upright stem, drawn per orb */
static void pw_draw_power_icon(stlxgfx_surface_t* s, float cx, float cy,
                               float r, float thickness, uint32_t color) {
    pw_arc(s, cx, cy, r, thickness, 0.085f, 0.83f, color);
    pw_disc(s, cx, cy - r * 0.98f, thickness * 0.5f, color);
    pw_disc(s, cx, cy - r * 0.22f, thickness * 0.5f, color);
    for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
        float y = cy - r * 0.98f + t * (r * 0.76f);
        pw_disc(s, cx, y, thickness * 0.5f, color);
    }
}

/* The restart glyph: a broken ring closed by an arrow head */
static void pw_draw_restart_icon(stlxgfx_surface_t* s, float cx, float cy,
                                 float r, float thickness, uint32_t color) {
    const float two_pi = 6.28318530718f;
    const float start_turn = 0.10f;
    const float sweep_turn = 0.80f;

    pw_arc(s, cx, cy, r, thickness, start_turn, sweep_turn, color);

    /* The head sits on the sweep end and points along the tangent, so the
     * ring reads as rotation rather than a detached triangle */
    float th = (start_turn + sweep_turn) * two_pi;
    float px = cx + r * sinf(th);
    float py = cy - r * cosf(th);
    float tx = cosf(th);
    float ty = sinf(th);
    float len = thickness * 2.4f;
    float half = thickness * 1.5f;

    pw_triangle(s, px + tx * len, py + ty * len,
                px + ty * half, py - tx * half,
                px - ty * half, py + tx * half, color);
}

void stlxdm_power_draw_overlay(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx) {
    if (pw->state == STLXDM_POWER_CLOSED) {
        return;
    }

    stlxgfx_surface_t* s = ctx->target;

    /* Past this point the screen is going black anyway, so it is filled
     * opaquely rather than blended, and the light finishes on top */
    if (pw->state == STLXDM_POWER_COMMITTING &&
        pw_phase(pw, PW_COMMIT_MS) >= PW_BLACKOUT_AT) {
        stlxgfx_fill_rect(s, 0, 0, pw->fb_width, pw->fb_height, 0xFF000000u);
        stlxdm_power_draw_orbs(pw, ctx);
        return;
    }

    /* The dim lands at full strength immediately. Fading it would mean
     * blending the whole screen every frame, which costs more than the
     * reveal is worth, and the orbs still carry the motion. */
    pw_dim_screen(s, PW_DIM_COLOR);

    const char* hint = "Hold to confirm     Esc to cancel";
    uint32_t hw = 0, hh = 0;
    stlxgfx_ctx_text_size(hint, PW_HINT_FONT, &hw, &hh);
    float hy = (float)pw->fb_height * 0.46f + (float)PW_ORB_RADIUS + 88.0f;
    stlxgfx_ctx_draw_text(ctx, (int32_t)(((float)pw->fb_width - (float)hw) * 0.5f),
                          (int32_t)hy, hint, PW_HINT_FONT, PW_HINT_COLOR);

    /* Everything above is identical on every later frame, so it is kept and
     * those frames only repaint the orb boxes over a copy of it */
    if (!pw->backdrop_valid) {
        if (!pw->backdrop) {
            pw->backdrop = stlxgfx_create_surface(pw->fb_width, pw->fb_height,
                                                   32, 16, 8, 0);
        }
        if (pw->backdrop) {
            stlxgfx_blit(pw->backdrop, 0, 0, s, 0, 0,
                         pw->fb_width, pw->fb_height);
            pw->backdrop_valid = 1;
        }
    }

    stlxdm_power_draw_orbs(pw, ctx);
}

void stlxdm_power_draw_orbs(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx) {
    stlxgfx_surface_t* s = ctx->target;

    if (pw->state == STLXDM_POWER_COMMITTING) {
        float t = pw_phase(pw, PW_COMMIT_MS);
        float cx, cy;
        pw_orb_center(pw, pw->commit_choice, &cx, &cy);
        uint32_t accent = pw->commit_choice == STLXDM_POWER_SHUTDOWN
            ? PW_SHUTDOWN_ACCENT : PW_RESTART_ACCENT;

        /* The chosen light flares, then contracts to a point and winks out,
         * which also covers the latency of the power call that follows */
        float flare = t < 0.18f ? (t / 0.18f) : 1.0f;
        float shrink = t < 0.18f ? 1.0f : (1.0f - (t - 0.18f) / 0.82f);
        if (shrink < 0.0f) shrink = 0.0f;

        float core = (4.0f + 10.0f * flare) * shrink;
        float halo = (float)PW_ORB_RADIUS * 0.92f * shrink;
        float veil = pw_ease_in(t / PW_BLACKOUT_AT);
        if (veil > 1.0f) veil = 1.0f;

        /* Darkening is confined to the light's own box so the rest of the
         * screen keeps the cached pixels until the final blackout */
        int32_t bx, by;
        uint32_t bw, bh;
        stlxdm_power_orb_box(pw, pw->commit_choice, &bx, &by, &bw, &bh);
        stlxgfx_fill_rect_blend(s, bx, by, bw, bh,
                                pw_with_alpha(0xFF000000u, veil));

        if (halo > 0.5f) {
            pw_glow(s, cx, cy, core, halo, pw_with_alpha(accent, 0.55f));
        }
        if (core > 0.4f) {
            pw_disc(s, cx, cy, core, pw_with_alpha(0xFFFFFFFFu, 0.95f));
        }
        return;
    }

    float reveal = 1.0f;
    if (pw->state == STLXDM_POWER_OPENING) {
        reveal = pw_ease_out(pw_phase(pw, PW_OPEN_MS));
    } else if (pw->state == STLXDM_POWER_CLOSING) {
        reveal = 1.0f - pw_phase(pw, PW_CLOSE_MS);
    }

    for (int i = 0; i < 2; i++) {
        float cx, cy;
        pw_orb_center(pw, i, &cx, &cy);

        /* The second orb trails the first so the pair reads as separating */
        float local = reveal;
        if (pw->state == STLXDM_POWER_OPENING && i == STLXDM_POWER_SHUTDOWN) {
            float t = pw_phase(pw, PW_OPEN_MS + PW_STAGGER_MS);
            float shifted = (t * (PW_OPEN_MS + PW_STAGGER_MS) - PW_STAGGER_MS)
                          / (float)PW_OPEN_MS;
            if (shifted < 0.0f) shifted = 0.0f;
            local = pw_ease_out(shifted);
        }

        float mid_x = (float)pw->fb_width * 0.5f;
        cx = mid_x + (cx - mid_x) * (0.55f + 0.45f * local);
        float r = (float)PW_ORB_RADIUS * (0.74f + 0.26f * local);

        int hovered = (pw->hover_choice == i);
        int holding = (pw->press_choice == i);
        uint32_t accent = i == STLXDM_POWER_SHUTDOWN ? PW_SHUTDOWN_ACCENT
                                                     : PW_RESTART_ACCENT;

        if (hovered || holding) {
            pw_glow(s, cx, cy, r, r + 26.0f,
                    pw_with_alpha(accent, 0.20f * local));
        }

        pw_disc(s, cx, cy, r, pw_with_alpha(
            hovered ? PW_ORB_FILL_HOVER : PW_ORB_FILL, local));
        pw_arc(s, cx, cy, r - 1.0f, 1.6f, 0.0f, 1.0f,
               pw_with_alpha(hovered ? accent : PW_ORB_EDGE, local));

        uint32_t glyph = pw_with_alpha(hovered ? accent : PW_LABEL_BRIGHT,
                                       local);
        if (i == STLXDM_POWER_SHUTDOWN) {
            pw_draw_power_icon(s, cx, cy, r * 0.42f, 3.4f, glyph);
        } else {
            pw_draw_restart_icon(s, cx, cy, r * 0.42f, 3.4f, glyph);
        }

        /* Hold progress winds around the orb and unwinds if released */
        if (holding) {
            uint64_t held = pw_now_ns() - pw->hold_start_ns;
            float p = (float)held / ((float)PW_HOLD_MS * 1000000.0f);
            if (p > 1.0f) p = 1.0f;
            pw_arc(s, cx, cy, r + 9.0f, 3.2f, 0.0f, p,
                   pw_with_alpha(accent, 0.95f));
        }

        uint32_t tw = 0, th = 0;
        stlxgfx_ctx_text_size(g_choice_label[i], PW_LABEL_FONT, &tw, &th);
        stlxgfx_ctx_draw_text(ctx, (int32_t)(cx - (float)tw * 0.5f),
                              (int32_t)(cy + r + 24.0f), g_choice_label[i],
                              PW_LABEL_FONT,
                              pw_with_alpha(hovered ? PW_LABEL_BRIGHT
                                                    : PW_LABEL_DIM, local));
    }
}

void stlxdm_power_run_action(stlxdm_power_t* pw) {
    if (!pw->action_ready) {
        return;
    }

    pw->action_ready = 0;
    int cmd = pw->commit_choice == STLXDM_POWER_SHUTDOWN ? RB_POWER_OFF
                                                         : RB_AUTOBOOT;
    reboot(cmd);

    /* Only reached when the platform cannot do it, so return to the desktop */
    printf("stlxdm: power operation unsupported\r\n");
    pw->state = STLXDM_POWER_CLOSED;
    pw->commit_choice = -1;
}
