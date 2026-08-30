/* The power overlay and its dock star. Orbs, glows, rings, and the
 * collapse are anti-aliased coverage drawing straight onto the
 * backbuffer, animated by the paced compose tick while any phase or
 * hold is in flight and costing nothing once settled.
 */
#include "power.hpp"

#include <stlxgfx/font.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/reboot.h>
#include <unistd.h>

/* Timings. The reveal stays short so the orbs are never something the
 * user waits on, and hit testing uses the settled layout from frame
 * one. */
constexpr uint32_t PW_OPEN_MS = 160;
constexpr uint32_t PW_CLOSE_MS = 110;
constexpr uint32_t PW_HOLD_MS = 700;
constexpr uint32_t PW_COMMIT_MS = 560;
constexpr uint32_t PW_STAGGER_MS = 40;

constexpr int32_t PW_RESTART = 0;
constexpr int32_t PW_SHUTDOWN = 1;

constexpr int32_t PW_ORB_RADIUS = 58;
constexpr int32_t PW_ORB_GAP = 216;
constexpr float PW_BLACKOUT_AT = 0.82f;
constexpr int32_t PW_STAR_MARGIN = 34;
constexpr int32_t PW_STAR_CORE = 6;
constexpr int32_t PW_STAR_GLOW = 13;

constexpr uint32_t PW_DIM_COLOR = 0xE60A0A12;
constexpr uint32_t PW_ORB_FILL = 0xFF181826;
constexpr uint32_t PW_ORB_FILL_HOVER = 0xFF20203A;
constexpr uint32_t PW_ORB_EDGE = 0xFF45475A;
constexpr uint32_t PW_LABEL_DIM = 0xFF9399B2;
constexpr uint32_t PW_LABEL_BRIGHT = 0xFFCDD6F4;
constexpr uint32_t PW_HINT_COLOR = 0xFF7F849C;
constexpr uint32_t PW_RESTART_ACCENT = 0xFF89B4FA;
constexpr uint32_t PW_SHUTDOWN_ACCENT = 0xFFF38BA8;
constexpr uint32_t PW_STAR_COLOR = 0xFFBAC2DE;
constexpr uint32_t PW_LABEL_FONT = 15;
constexpr uint32_t PW_HINT_FONT = 12;

static const char* const g_choice_label[2] = { "Restart", "Shut Down" };

/* Faces for the orb labels and the hint line, opened once */
static stlxgfx_font* g_label_font = nullptr;
static stlxgfx_font_metrics g_label_fm = {};
static stlxgfx_font* g_hint_font = nullptr;
static stlxgfx_font_metrics g_hint_fm = {};

static uint64_t pw_now_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(ts.tv_nsec);
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
    float scaled = static_cast<float>(a) * scale;
    if (scaled < 0.0f) scaled = 0.0f;
    if (scaled > 255.0f) scaled = 255.0f;

    return (color & 0x00FFFFFFu) | (static_cast<uint32_t>(scaled) << 24);
}

/* Copies the source while fading it toward black in the same pass,
 * so a collapse frame walks every pixel exactly once */
static void pw_blit_faded(stlxgfx_surface_t* dst, stlxgfx_surface_t* src,
                          float veil) {
    if (veil < 0.0f) veil = 0.0f;
    if (veil > 1.0f) veil = 1.0f;

    uint32_t keep = static_cast<uint32_t>((1.0f - veil) * 256.0f);
    if (keep > 256u) {
        keep = 256u;
    }

    uint32_t h = dst->height < src->height ? dst->height : src->height;
    uint32_t w = dst->width < src->width ? dst->width : src->width;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* drow = reinterpret_cast<uint32_t*>(
            dst->pixels + static_cast<size_t>(y) * dst->pitch);
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(
            src->pixels + static_cast<size_t>(y) * src->pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = srow[x];
            uint32_t r = (((c >> 16) & 0xFFu) * keep) >> 8;
            uint32_t g = (((c >> 8) & 0xFFu) * keep) >> 8;
            uint32_t b = ((c & 0xFFu) * keep) >> 8;
            drow[x] = (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}

/* Anti-aliased filled disc. Only the one pixel edge band pays for a
 * square root, the interior and outside compare squared distances. */
static void pw_disc(stlxgfx_surface_t* s, float cx, float cy,
                    float radius, uint32_t color) {
    if (radius <= 0.0f) {
        return;
    }

    int32_t x0 = static_cast<int32_t>(floorf(cx - radius - 1.0f));
    int32_t x1 = static_cast<int32_t>(ceilf(cx + radius + 1.0f));
    int32_t y0 = static_cast<int32_t>(floorf(cy - radius - 1.0f));
    int32_t y1 = static_cast<int32_t>(ceilf(cy + radius + 1.0f));

    float r_out = radius + 0.5f;
    float r_in = radius - 0.5f;
    float r_out2 = r_out * r_out;
    float r_in2 = r_in > 0.0f ? r_in * r_in : -1.0f;
    for (int32_t y = y0; y <= y1; y++) {
        float dy = static_cast<float>(y) + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = static_cast<float>(x) + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= r_out2) {
                continue;
            }

            if (d2 <= r_in2) {
                stlxgfx_blend_coverage(s, x, y, color, 255);
                continue;
            }

            float cov = radius + 0.5f - sqrtf(d2);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            stlxgfx_blend_coverage(s, x, y, color,
                                   static_cast<uint8_t>(cov * 255.0f));
        }
    }
}

/* Soft radial halo fading from the core radius out to the glow edge */
static void pw_glow(stlxgfx_surface_t* s, float cx, float cy,
                    float r_core, float r_glow, uint32_t color) {
    if (r_glow <= r_core) {
        return;
    }

    int32_t x0 = static_cast<int32_t>(floorf(cx - r_glow - 1.0f));
    int32_t x1 = static_cast<int32_t>(ceilf(cx + r_glow + 1.0f));
    int32_t y0 = static_cast<int32_t>(floorf(cy - r_glow - 1.0f));
    int32_t y1 = static_cast<int32_t>(ceilf(cy + r_glow + 1.0f));

    float span = r_glow - r_core;
    float r_glow2 = r_glow * r_glow;
    float r_core2 = r_core * r_core;
    for (int32_t y = y0; y <= y1; y++) {
        float dy = static_cast<float>(y) + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = static_cast<float>(x) + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= r_glow2) {
                continue;
            }

            float f = 1.0f;
            if (d2 > r_core2) {
                f = (r_glow - sqrtf(d2)) / span;
            }
            f = f * f;
            stlxgfx_blend_coverage(s, x, y, color,
                                   static_cast<uint8_t>(f * 255.0f));
        }
    }
}

/* Anti-aliased arc. Angles are turns clockwise from twelve o'clock,
 * which keeps the icon and progress geometry readable. */
static void pw_arc(stlxgfx_surface_t* s, float cx, float cy, float radius,
                   float thickness, float start_turn, float sweep_turn,
                   uint32_t color) {
    if (sweep_turn <= 0.0f || thickness <= 0.0f) {
        return;
    }

    float half = thickness * 0.5f;
    float r_out = radius + half;
    int32_t x0 = static_cast<int32_t>(floorf(cx - r_out - 1.0f));
    int32_t x1 = static_cast<int32_t>(ceilf(cx + r_out + 1.0f));
    int32_t y0 = static_cast<int32_t>(floorf(cy - r_out - 1.0f));
    int32_t y1 = static_cast<int32_t>(ceilf(cy + r_out + 1.0f));

    /* The band is thin, so squared bounds reject most of the box
     * before any square root or angle work happens */
    float band_out = radius + half + 0.5f;
    float band_in = radius - half - 0.5f;
    float band_out2 = band_out * band_out;
    float band_in2 = band_in > 0.0f ? band_in * band_in : -1.0f;

    const float two_pi = 6.28318530718f;
    for (int32_t y = y0; y <= y1; y++) {
        float dy = static_cast<float>(y) + 0.5f - cy;
        float dy2 = dy * dy;
        for (int32_t x = x0; x <= x1; x++) {
            float dx = static_cast<float>(x) + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= band_out2 || d2 <= band_in2) {
                continue;
            }

            float d = sqrtf(d2);
            float cov = (half + 0.5f) - fabsf(d - radius);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float ang = atan2f(dx, -dy);
            if (ang < 0.0f) {
                ang += two_pi;
            }
            float turn = ang / two_pi - start_turn;
            while (turn < 0.0f) {
                turn += 1.0f;
            }
            if (turn > sweep_turn) {
                continue;
            }

            stlxgfx_blend_coverage(s, x, y, color,
                                   static_cast<uint8_t>(cov * 255.0f));
        }
    }
}

/* Filled triangle from three points, the restart arrow head */
static void pw_triangle(stlxgfx_surface_t* s, float ax, float ay,
                        float bx, float by, float cx, float cy,
                        uint32_t color) {
    float min_x = fminf(ax, fminf(bx, cx));
    float max_x = fmaxf(ax, fmaxf(bx, cx));
    float min_y = fminf(ay, fminf(by, cy));
    float max_y = fmaxf(ay, fmaxf(by, cy));

    for (int32_t y = static_cast<int32_t>(floorf(min_y));
         y <= static_cast<int32_t>(ceilf(max_y)); y++) {
        for (int32_t x = static_cast<int32_t>(floorf(min_x));
             x <= static_cast<int32_t>(ceilf(max_x)); x++) {
            float px = static_cast<float>(x) + 0.5f;
            float py = static_cast<float>(y) + 0.5f;
            float d0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float d1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float d2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            bool neg = d0 < 0.0f || d1 < 0.0f || d2 < 0.0f;
            bool pos = d0 > 0.0f || d1 > 0.0f || d2 > 0.0f;
            if (neg && pos) {
                continue;
            }
            stlxgfx_blend_coverage(s, x, y, color, 255);
        }
    }
}

/* Whole surface dim in one fixed point pass, the surface is always
 * the packed 32bpp backbuffer or a view into it */
static void pw_dim(stlxgfx_surface_t* s, uint32_t color) {
    uint32_t a = (color >> 24) & 0xFFu;
    if (a == 0) {
        return;
    }

    uint32_t inv = 255u - a;
    uint32_t sr = ((color >> 16) & 0xFFu) * a;
    uint32_t sg = ((color >> 8) & 0xFFu) * a;
    uint32_t sb = (color & 0xFFu) * a;
    for (uint32_t y = 0; y < s->height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(
            s->pixels + static_cast<size_t>(y) * s->pitch);
        for (uint32_t x = 0; x < s->width; x++) {
            uint32_t d = row[x];
            uint32_t r = ((((d >> 16) & 0xFFu) * inv) + sr) >> 8;
            uint32_t g = ((((d >> 8) & 0xFFu) * inv) + sg) >> 8;
            uint32_t b = (((d & 0xFFu) * inv) + sb) >> 8;
            row[x] = (d & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}

/* The power glyph: a broken ring with an upright stem */
static void pw_power_icon(stlxgfx_surface_t* s, float cx, float cy,
                          float r, float thickness, uint32_t color) {
    pw_arc(s, cx, cy, r, thickness, 0.085f, 0.83f, color);
    pw_disc(s, cx, cy - r * 0.98f, thickness * 0.5f, color);
    pw_disc(s, cx, cy - r * 0.22f, thickness * 0.5f, color);

    for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
        float y = cy - r * 0.98f + t * (r * 0.76f);
        pw_disc(s, cx, y, thickness * 0.5f, color);
    }
}

/* The restart glyph: a broken ring closed by an arrow head riding
 * the sweep's tangent so the ring reads as rotation */
static void pw_restart_icon(stlxgfx_surface_t* s, float cx, float cy,
                            float r, float thickness, uint32_t color) {
    const float two_pi = 6.28318530718f;
    const float start_turn = 0.10f;
    const float sweep_turn = 0.80f;

    pw_arc(s, cx, cy, r, thickness, start_turn, sweep_turn, color);

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

/* Centered baseline text through the given face */
static void pw_text_centered(stlxgfx_surface_t* s, stlxgfx_font* font,
                             const stlxgfx_font_metrics& fm, float cx,
                             float top_y, const char* text,
                             uint32_t color) {
    int32_t tw = stlxgfx_text_width(font, text, strlen(text));
    int32_t x = static_cast<int32_t>(cx) - tw / 2;

    stlxgfx_draw_text(s, font, x,
                      static_cast<int32_t>(top_y) + fm.ascent,
                      text, strlen(text), color);
}

int dm_power::init(uint32_t screen_w, uint32_t screen_h,
                   uint32_t taskbar_height) {
    m_width = screen_w;
    m_height = screen_h;

    int32_t bar_y = static_cast<int32_t>(screen_h - taskbar_height);
    m_star_cx = static_cast<int32_t>(screen_w) - PW_STAR_MARGIN;
    m_star_cy = bar_y + static_cast<int32_t>(taskbar_height) / 2;

    g_label_font = stlxgfx_font_open(STLXGFX_FONT_PATH, PW_LABEL_FONT);
    g_hint_font = stlxgfx_font_open(STLXGFX_FONT_PATH, PW_HINT_FONT);
    if (!g_label_font || !g_hint_font) {
        return -1;
    }

    stlxgfx_font_metrics_get(g_label_font, &g_label_fm);
    stlxgfx_font_metrics_get(g_hint_font, &g_hint_fm);

    /* Reserved and touched up front, so the first collapse pays for
     * neither the allocation nor faulting a screen of pages in */
    m_backdrop = stlxgfx_create_surface(screen_w, screen_h, 32, 16, 8, 0);
    if (m_backdrop) {
        stlxgfx_clear(m_backdrop, 0xFF000000u);
    }

    return 0;
}

void dm_power::shutdown() {
    stlxgfx_destroy_surface(m_backdrop);
    m_backdrop = nullptr;

    stlxgfx_font_close(g_label_font);
    stlxgfx_font_close(g_hint_font);
    g_label_font = nullptr;
    g_hint_font = nullptr;
}

bool dm_power::collapsing() const {
    return m_state == state::committing && m_backdrop_valid &&
           phase(PW_COMMIT_MS) < PW_BLACKOUT_AT;
}

/* Progress through the current phase, clamped once it has elapsed */
float dm_power::phase(uint32_t duration_ms) const {
    if (duration_ms == 0) {
        return 1.0f;
    }

    uint64_t elapsed = pw_now_ns() - m_phase_start_ns;
    float t = static_cast<float>(elapsed)
            / (static_cast<float>(duration_ms) * 1000000.0f);

    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

/* Settled centre of an orb. Hit testing always uses this, so the
 * choices are clickable from the first frame of the reveal. */
void dm_power::orb_center(int32_t choice, float* cx, float* cy) const {
    float mid_x = static_cast<float>(m_width) * 0.5f;
    float offset = static_cast<float>(PW_ORB_GAP) * 0.5f;

    *cx = choice == PW_RESTART ? mid_x - offset : mid_x + offset;
    *cy = static_cast<float>(m_height) * 0.46f;
}

int32_t dm_power::choice_at(int32_t x, int32_t y) const {
    for (int32_t i = 0; i < 2; i++) {
        float cx, cy;
        orb_center(i, &cx, &cy);

        float dx = static_cast<float>(x) - cx;
        float dy = static_cast<float>(y) - cy;
        float r = static_cast<float>(PW_ORB_RADIUS) + 6.0f;
        if (dx * dx + dy * dy <= r * r) {
            return i;
        }
    }

    return -1;
}

damage_list::rect dm_power::orb_box(int32_t choice) const {
    float cx, cy;
    orb_center(choice, &cx, &cy);

    int32_t pad = PW_ORB_RADIUS + 44;

    return { static_cast<int32_t>(cx) - pad,
             static_cast<int32_t>(cy) - pad,
             2 * pad, pad + PW_ORB_RADIUS + 62 };
}

void dm_power::open() {
    if (m_state == state::opening || m_state == state::open) {
        return;
    }

    m_state = state::opening;
    m_phase_start_ns = pw_now_ns();
    m_hover = -1;
    m_press = -1;
    m_backdrop_valid = false;
}

void dm_power::dismiss() {
    if (m_state != state::open && m_state != state::opening) {
        return;
    }

    m_state = state::closing;
    m_phase_start_ns = pw_now_ns();
    m_hover = -1;
    m_press = -1;
}

void dm_power::update() {
    switch (m_state) {
    case state::opening:
        if (phase(PW_OPEN_MS) >= 1.0f) {
            m_state = state::open;
            m_phase_start_ns = pw_now_ns();
        }
        break;
    case state::closing:
        if (phase(PW_CLOSE_MS) >= 1.0f) {
            m_state = state::closed;
            m_backdrop_valid = false;
        }
        break;
    case state::committing:
        if (phase(PW_COMMIT_MS) >= 1.0f) {
            m_action_ready = true;
        }
        break;
    default:
        break;
    }

    if (m_state != state::open || m_press < 0) {
        return;
    }

    uint64_t held = pw_now_ns() - m_hold_start_ns;
    if (held >= static_cast<uint64_t>(PW_HOLD_MS) * 1000000ull) {
        m_commit = m_press;
        m_press = -1;
        m_state = state::committing;
        m_phase_start_ns = pw_now_ns();
    }
}

void dm_power::on_motion(int32_t x, int32_t y) {
    if (m_state == state::closed) {
        m_star_hover = star_hit(x, y);
        return;
    }

    m_star_hover = false;
    if (m_state != state::opening && m_state != state::open) {
        return;
    }

    m_hover = choice_at(x, y);

    /* Sliding off the orb abandons the hold instead of confirming
     * blind */
    if (m_press >= 0 && m_hover != m_press) {
        m_press = -1;
    }
}

void dm_power::on_press(int32_t x, int32_t y) {
    if (m_state != state::opening && m_state != state::open) {
        return;
    }

    int32_t choice = choice_at(x, y);
    if (choice < 0) {
        dismiss();
        return;
    }

    m_press = choice;
    m_hold_start_ns = pw_now_ns();
}

void dm_power::on_release() {
    if (m_state == state::opening || m_state == state::open) {
        m_press = -1;
    }
}

bool dm_power::star_hit(int32_t x, int32_t y) const {
    int32_t dx = x - m_star_cx;
    int32_t dy = y - m_star_cy;
    int32_t r = PW_STAR_GLOW + 4;

    return dx * dx + dy * dy <= r * r;
}

void dm_power::draw_star(stlxgfx_surface_t* back,
                         const damage_list::rect& clip) {
    if (m_state != state::closed || clip.w <= 0 || clip.h <= 0) {
        return;
    }

    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(clip.y) * back->pitch
            + static_cast<uint32_t>(clip.x) * 4,
        static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
        back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    float cx = static_cast<float>(m_star_cx - clip.x);
    float cy = static_cast<float>(m_star_cy - clip.y);
    float boost = m_star_hover ? 1.0f : 0.0f;

    uint32_t halo = pw_with_alpha(PW_STAR_COLOR, 0.20f + 0.22f * boost);
    pw_glow(view, cx, cy, 1.5f,
            static_cast<float>(PW_STAR_GLOW) + 2.0f * boost, halo);
    pw_disc(view, cx, cy,
            static_cast<float>(PW_STAR_CORE) * (1.0f + 0.08f * boost),
            pw_with_alpha(PW_STAR_COLOR, 0.75f + 0.25f * boost));

    if (m_star_hover) {
        pw_arc(view, cx, cy, static_cast<float>(PW_STAR_GLOW) - 1.0f,
               1.5f, 0.0f, 1.0f, pw_with_alpha(PW_STAR_COLOR, 0.55f));
    }

    stlxgfx_destroy_surface(view);
}

void dm_power::draw_orbs(stlxgfx_surface_t* s, int32_t ox, int32_t oy) {
    if (m_state == state::committing) {
        draw_collapse_light(s, ox, oy);
        return;
    }

    float reveal = 1.0f;
    if (m_state == state::opening) {
        reveal = pw_ease_out(phase(PW_OPEN_MS));
    } else if (m_state == state::closing) {
        reveal = 1.0f - phase(PW_CLOSE_MS);
    }

    for (int32_t i = 0; i < 2; i++) {
        float cx, cy;
        orb_center(i, &cx, &cy);
        cx -= static_cast<float>(ox);
        cy -= static_cast<float>(oy);

        /* The second orb trails the first so the pair reads as
         * separating */
        float local = reveal;
        if (m_state == state::opening && i == PW_SHUTDOWN) {
            float t = phase(PW_OPEN_MS + PW_STAGGER_MS);
            float shifted =
                (t * static_cast<float>(PW_OPEN_MS + PW_STAGGER_MS)
                 - static_cast<float>(PW_STAGGER_MS))
                / static_cast<float>(PW_OPEN_MS);
            if (shifted < 0.0f) {
                shifted = 0.0f;
            }
            local = pw_ease_out(shifted);
        }

        float mid_x = static_cast<float>(m_width) * 0.5f
                    - static_cast<float>(ox);
        cx = mid_x + (cx - mid_x) * (0.55f + 0.45f * local);
        float r = static_cast<float>(PW_ORB_RADIUS)
                * (0.74f + 0.26f * local);

        bool hovered = m_hover == i;
        bool holding = m_press == i;
        uint32_t accent = i == PW_SHUTDOWN ? PW_SHUTDOWN_ACCENT
                                           : PW_RESTART_ACCENT;

        if (hovered || holding) {
            pw_glow(s, cx, cy, r, r + 26.0f,
                    pw_with_alpha(accent, 0.20f * local));
        }

        pw_disc(s, cx, cy, r,
                pw_with_alpha(hovered ? PW_ORB_FILL_HOVER : PW_ORB_FILL,
                              local));
        pw_arc(s, cx, cy, r - 1.0f, 1.6f, 0.0f, 1.0f,
               pw_with_alpha(hovered ? accent : PW_ORB_EDGE, local));

        uint32_t glyph = pw_with_alpha(
            hovered ? accent : PW_LABEL_BRIGHT, local);
        if (i == PW_SHUTDOWN) {
            pw_power_icon(s, cx, cy, r * 0.42f, 3.4f, glyph);
        } else {
            pw_restart_icon(s, cx, cy, r * 0.42f, 3.4f, glyph);
        }

        /* Hold progress winds around the orb and unwinds if released */
        if (holding) {
            uint64_t held = pw_now_ns() - m_hold_start_ns;
            float p = static_cast<float>(held)
                    / (static_cast<float>(PW_HOLD_MS) * 1000000.0f);
            if (p > 1.0f) {
                p = 1.0f;
            }
            pw_arc(s, cx, cy, r + 9.0f, 3.2f, 0.0f, p,
                   pw_with_alpha(accent, 0.95f));
        }

        pw_text_centered(s, g_label_font, g_label_fm, cx, cy + r + 24.0f,
                         g_choice_label[i],
                         pw_with_alpha(hovered ? PW_LABEL_BRIGHT
                                               : PW_LABEL_DIM, local));
    }
}

void dm_power::draw_overlay(stlxgfx_surface_t* back,
                            const damage_list::rect& clip) {
    if (m_state == state::closed || clip.w <= 0 || clip.h <= 0) {
        return;
    }

    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(clip.y) * back->pitch
            + static_cast<uint32_t>(clip.x) * 4,
        static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
        back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    /* Past the blackout point the screen is going black anyway, so it
     * is filled opaquely and only the light finishes on top */
    if (m_state == state::committing &&
        phase(PW_COMMIT_MS) >= PW_BLACKOUT_AT) {
        stlxgfx_fill_rect(view, 0, 0,
                          static_cast<uint32_t>(clip.w),
                          static_cast<uint32_t>(clip.h), 0xFF000000u);
        draw_collapse_light(view, clip.x, clip.y);
        stlxgfx_destroy_surface(view);
        return;
    }

    /* The dim lands at full strength immediately. Fading it would
     * blend the whole screen per frame, and the orbs carry the
     * motion. */
    pw_dim(view, PW_DIM_COLOR);

    const char* hint = "Hold to confirm     Esc to cancel";
    float hy = static_cast<float>(m_height) * 0.46f
             + static_cast<float>(PW_ORB_RADIUS) + 88.0f;
    pw_text_centered(view, g_hint_font, g_hint_fm,
                     static_cast<float>(m_width) * 0.5f
                         - static_cast<float>(clip.x),
                     hy - static_cast<float>(clip.y), hint,
                     PW_HINT_COLOR);

    /* A full screen frame with the dim and hint in place is exactly
     * the collapse backdrop, captured before the orbs go on top */
    bool full = clip.x == 0 && clip.y == 0 &&
                clip.w == static_cast<int32_t>(m_width) &&
                clip.h == static_cast<int32_t>(m_height);
    if (full && !m_backdrop_valid && m_backdrop) {
        stlxgfx_blit(m_backdrop, 0, 0, back, 0, 0, m_width, m_height);
        m_backdrop_valid = true;
    }

    draw_orbs(view, clip.x, clip.y);
    stlxgfx_destroy_surface(view);
}

/* The chosen light flares, then contracts to a point and winks out,
 * which also covers the latency of the power call that follows */
void dm_power::draw_collapse_light(stlxgfx_surface_t* s, int32_t ox,
                                   int32_t oy) {
    float t = phase(PW_COMMIT_MS);
    float cx, cy;
    orb_center(m_commit, &cx, &cy);
    cx -= static_cast<float>(ox);
    cy -= static_cast<float>(oy);

    uint32_t accent = m_commit == PW_SHUTDOWN ? PW_SHUTDOWN_ACCENT
                                              : PW_RESTART_ACCENT;
    float flare = t < 0.18f ? (t / 0.18f) : 1.0f;
    float shrink = t < 0.18f ? 1.0f : (1.0f - (t - 0.18f) / 0.82f);
    if (shrink < 0.0f) {
        shrink = 0.0f;
    }

    float core = (4.0f + 10.0f * flare) * shrink;
    float halo = static_cast<float>(PW_ORB_RADIUS) * 0.92f * shrink;
    if (halo > 0.5f) {
        pw_glow(s, cx, cy, core, halo, pw_with_alpha(accent, 0.55f));
    }
    if (core > 0.4f) {
        pw_disc(s, cx, cy, core, pw_with_alpha(0xFFFFFFFFu, 0.95f));
    }
}

void dm_power::draw_collapse(stlxgfx_surface_t* back) {
    float t = phase(PW_COMMIT_MS);
    float veil = pw_ease_in(t / PW_BLACKOUT_AT);

    pw_blit_faded(back, m_backdrop, veil);
    draw_collapse_light(back, 0, 0);
}

void dm_power::run_action() {
    if (!m_action_ready) {
        return;
    }

    m_action_ready = false;
    int cmd = m_commit == PW_SHUTDOWN ? RB_POWER_OFF : RB_AUTOBOOT;
    reboot(cmd);

    /* Only reached when the platform refused, back to the desktop */
    printf("stlxdm: power operation unsupported\r\n");
    m_state = state::closed;
    m_commit = -1;
    m_backdrop_valid = false;
}
