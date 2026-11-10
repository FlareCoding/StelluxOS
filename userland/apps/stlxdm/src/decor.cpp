#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/blend.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cmath>
#include <cstring>

constexpr uint32_t CLOSE_BG           = 0xFF45475A;
constexpr uint32_t CLOSE_BG_HOVER     = 0xFFF38BA8;
constexpr uint32_t CLOSE_BG_PRESS     = 0xFFD06080;
constexpr uint32_t CLOSE_FG           = 0xFFBAC2DE;
constexpr uint32_t CLOSE_FG_HOVER     = 0xFFFFFFFF;
constexpr int32_t  TITLE_INSET        = 14;  /* from the frame's left edge */
constexpr uint32_t OUTLINE_COLOR      = 0xFF89B4FA;

/* The focus rule is the top border of a box with the window's radius, so its inner
 * edge is a quarter ellipse, which each pixel measures from a grid of samples */
constexpr int32_t RULE_W = STLXTHEME_FOCUS_RULE_WIDTH;
constexpr int32_t RULE_SAMPLES = 16;                               /* per side of a pixel */
constexpr int32_t RULE_SAMPLE_COUNT = RULE_SAMPLES * RULE_SAMPLES;
constexpr int32_t RULE_SAMPLE_UNITS_PER_PIXEL = 2 * RULE_SAMPLES;  /* sample centers lie on the odd units */

/* Corner distances run through a table of squared half pixel offsets,
 * so the content arcs never need a square root per pixel */
constexpr int32_t SDF_Q_MAX = 2 * decor::CORNER_R;
constexpr int32_t SDF_LUT_N = 2 * SDF_Q_MAX * SDF_Q_MAX + 1;

/* A rounded rect in pixels */
struct ring_geom {
    int32_t x = 0, y = 0;
    int32_t w = 0, h = 0;
    int32_t r = 0;
};

/* The chrome face at the title size, opened once at startup */
static stlxgfx_font* g_font = nullptr;
static stlxgfx_font_metrics g_fm = {};
static uint16_t g_dist8[SDF_LUT_N];

/* Coverage of the box beneath the focus rule over the frame's top-left
 * corner square, by row then column */
static uint8_t g_below_rule[decor::CORNER_R][decor::CORNER_R];

static const dm_buffer* current_buffer(const dm_window& w) {
    if (w.current < 0) {
        return nullptr;
    }

    return &w.buffers[static_cast<size_t>(w.current)];
}

/* A writable view over the clip rect, so every primitive clips for
 * free. Coordinates shift into view space at the call sites. */
static stlxgfx_surface_t* clip_view(const decor::canvas& back,
                                    const damage_list::rect& clip) {
    if (clip.w <= 0 || clip.h <= 0) {
        return nullptr;
    }

    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back.surface->pixels + static_cast<uint32_t>(clip.y - back.y) * back.surface->pitch
            + static_cast<uint32_t>(clip.x - back.x) * 4,
        static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
        back.surface->pitch, 32, 16, 8, 0);
    if (view) {
        view->flags = back.surface->flags;
    }

    return view;
}

static void sdf_table_init() {
    for (int32_t k = 0; k < SDF_LUT_N; k++) {
        g_dist8[k] = static_cast<uint16_t>(sqrtf(static_cast<float>(k)) * 4.0f + 0.5f);
    }
}

/* Whether a point of the frame's top-left corner square, in sample units
 * from the square's top-left, lies in the box beneath the focus rule */
static bool is_below_rule(int64_t x, int64_t y) {
    int64_t radius_x = decor::CORNER_R;
    int64_t radius_y = decor::CORNER_R - RULE_W;
    int64_t dx = decor::CORNER_R * RULE_SAMPLE_UNITS_PER_PIXEL - x;
    int64_t dy = decor::CORNER_R * RULE_SAMPLE_UNITS_PER_PIXEL - y;
    int64_t extent = radius_x * radius_y * RULE_SAMPLE_UNITS_PER_PIXEL;

    return dx * dx * radius_y * radius_y + dy * dy * radius_x * radius_x <= extent * extent;
}

static uint8_t measure_below_rule(int32_t row, int32_t column) {
    int32_t inside = 0;
    for (int32_t sample = 0; sample < RULE_SAMPLE_COUNT; sample++) {
        int64_t x = column * RULE_SAMPLE_UNITS_PER_PIXEL + 2 * (sample % RULE_SAMPLES) + 1;
        int64_t y = row * RULE_SAMPLE_UNITS_PER_PIXEL + 2 * (sample / RULE_SAMPLES) + 1;
        inside += is_below_rule(x, y) ? 1 : 0;
    }

    return static_cast<uint8_t>((inside * 255 + RULE_SAMPLE_COUNT / 2) / RULE_SAMPLE_COUNT);
}

static void below_rule_table_init() {
    for (int32_t row = 0; row < decor::CORNER_R; row++) {
        for (int32_t column = 0; column < decor::CORNER_R; column++) {
            g_below_rule[row][column] = measure_below_rule(row, column);
        }
    }
}

/* Signed distance from a pixel center to the rounded rect edge, in
 * eighth pixels, negative inside. Far outside saturates. */
static inline int32_t rrect_dist8(const ring_geom& g, int32_t px, int32_t py) {
    int32_t ax = 2 * px + 1 - (2 * g.x + g.w);
    int32_t ay = 2 * py + 1 - (2 * g.y + g.h);
    if (ax < 0) ax = -ax;
    if (ay < 0) ay = -ay;

    int32_t qx = ax - (g.w - 2 * g.r);
    int32_t qy = ay - (g.h - 2 * g.r);
    int32_t r8 = g.r * 8;
    if (qx <= 0 && qy <= 0) {
        return (qx > qy ? qx : qy) * 4 - r8;
    }

    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    int32_t k = qx * qx + qy * qy;
    if (k >= SDF_LUT_N) {
        return SDF_Q_MAX * 8;
    }

    return static_cast<int32_t>(g_dist8[k]) - r8;
}

/* Edge coverage from a signed eighth pixel distance, 0 to 255 */
static inline uint32_t dist8_coverage(int32_t d8) {
    if (d8 <= -4) {
        return 255;
    }
    if (d8 >= 4) {
        return 0;
    }

    uint32_t c = static_cast<uint32_t>(4 - d8) * 32;
    return c > 255 ? 255 : c;
}

static inline uint32_t blend_px(uint32_t dst, uint32_t color, uint32_t a) {
    if (a >= 255) {
        return 0xFF000000u | (color & 0x00FFFFFFu);
    }

    uint32_t inv = 255 - a;
    uint32_t r = stlxgfx_blend_channel((dst >> 16) & 0xFF, ((color >> 16) & 0xFF) * a, inv);
    uint32_t g = stlxgfx_blend_channel((dst >> 8) & 0xFF, ((color >> 8) & 0xFF) * a, inv);
    uint32_t b = stlxgfx_blend_channel(dst & 0xFF, (color & 0xFF) * a, inv);

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t* back_row(stlxgfx_surface_t* back, int32_t y) {
    return reinterpret_cast<uint32_t*>(back->pixels + static_cast<uint32_t>(y) * back->pitch);
}

/* Fills the part of `r` inside `clip`, both in desktop coordinates */
static void fill_inside(const decor::canvas& back, const damage_list::rect& clip,
                        const damage_list::rect& r, uint32_t color) {
    damage_list::rect inside = damage_list::intersect(r, clip);
    if (inside.w <= 0 || inside.h <= 0) {
        return;
    }

    stlxgfx_fill_rect(back.surface, inside.x - back.x, inside.y - back.y,
                      static_cast<uint32_t>(inside.w), static_cast<uint32_t>(inside.h), color);
}

static uint32_t coverage_of(const ring_geom& g, int32_t x, int32_t y) {
    return dist8_coverage(rrect_dist8(g, x, y));
}

/* Coverage of the focus rule at a pixel of the frame `outer`, which the
 * frame itself covers by `frame_coverage` there */
static uint32_t rule_coverage(const ring_geom& outer, uint32_t frame_coverage, int32_t x, int32_t y) {
    int32_t row = y - outer.y;
    if (row >= decor::CORNER_R) {
        return 0;
    }

    int32_t column = std::min(x - outer.x, outer.x + outer.w - 1 - x);
    uint32_t below = row < RULE_W ? 0 : 255;
    if (column < decor::CORNER_R) {
        below = g_below_rule[row][column];
    }

    return frame_coverage > below ? frame_coverage - below : 0;
}

/* Blends the frame's material, title separator, edge and, on the focused
 * window, the focus rule over what `view` holds, each by its coverage */
static void draw_body(stlxgfx_surface_t* view, const damage_list::rect& area, const ring_geom& outer,
                      int32_t separator_y, const decor::chrome_state& st) {
    ring_geom inner = { outer.x + decor::BORDER, outer.y + decor::BORDER, outer.w - 2 * decor::BORDER,
                        outer.h - 2 * decor::BORDER, decor::CONTENT_R };
    uint32_t edge = st.focused ? STLXTHEME_RIM : STLXTHEME_RIM_DIM;
    damage_list::rect painted = damage_list::intersect(area, { outer.x, outer.y, outer.w, outer.h });

    for (int32_t y = painted.y; y < painted.y + painted.h; y++) {
        for (int32_t x = painted.x; x < painted.x + painted.w; x++) {
            uint32_t frame_coverage = coverage_of(outer, x, y);
            if (frame_coverage == 0) {
                continue;
            }

            int32_t vx = x - area.x;
            int32_t vy = y - area.y;
            stlxgfx_blend_coverage(view, vx, vy, decor::CONTENT_BG, static_cast<uint8_t>(frame_coverage));
            if (y == separator_y) {
                stlxgfx_blend_coverage(view, vx, vy, STLXTHEME_LINE_SUBTLE, static_cast<uint8_t>(frame_coverage));
            }

            uint32_t inner_coverage = coverage_of(inner, x, y);
            if (frame_coverage > inner_coverage) {
                stlxgfx_blend_coverage(view, vx, vy, edge, static_cast<uint8_t>(frame_coverage - inner_coverage));
            }

            uint32_t rule = st.focused ? rule_coverage(outer, frame_coverage, x, y) : 0;
            if (rule != 0) {
                stlxgfx_blend_coverage(view, vx, vy, STLXTHEME_ACCENT, static_cast<uint8_t>(rule));
            }
        }
    }
}

namespace decor {

int init() {
    g_font = stlxgfx_font_open(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE);
    if (!g_font) {
        return -1;
    }

    stlxgfx_font_metrics_get(g_font, &g_fm);
    sdf_table_init();
    below_rule_table_init();

    return 0;
}

corner_table corner_distances() {
    return { g_dist8, SDF_LUT_N, SDF_Q_MAX * 8 };
}

bool decorated(const dm_window& w) {
    return (w.flags & SWP_WF_BORDERLESS) == 0;
}

damage_list::rect frame_rect(const dm_window& w, int32_t x, int32_t y,
                             int32_t cw, int32_t ch) {
    if (!decorated(w)) {
        return { x, y, cw, ch };
    }

    return { x - BORDER, y - TITLE_H,
             cw + 2 * BORDER, ch + TITLE_H + BORDER };
}

damage_list::rect bounds(const dm_window& w) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return {};
    }

    return frame_rect(w, w.x, w.y, static_cast<int32_t>(b->width), static_cast<int32_t>(b->height));
}

void frame_strips(const dm_window& w, damage_list::rect (&strips)[FRAME_STRIPS]) {
    const dm_buffer* b = current_buffer(w);
    if (!b || !decorated(w)) {
        for (damage_list::rect& strip : strips) {
            strip = {};
        }

        return;
    }

    damage_list::rect outer = bounds(w);
    int32_t bw = static_cast<int32_t>(b->width);
    int32_t bh = static_cast<int32_t>(b->height);
    int32_t corner_y = w.y + (bh > CONTENT_R ? bh - CONTENT_R : 0);
    int32_t right_x = w.x + bw;

    strips[0] = { outer.x, outer.y, outer.w, w.y - outer.y };
    strips[1] = { outer.x, w.y, w.x - outer.x, corner_y - w.y };
    strips[2] = { right_x, w.y, outer.x + outer.w - right_x, corner_y - w.y };
    strips[3] = { outer.x, corner_y, outer.w, outer.y + outer.h - corner_y };
}

zone hit(const dm_window& w, int32_t x, int32_t y) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return zone::none;
    }

    bool resizable = decorated(w) && (w.flags & SWP_WF_RESIZABLE) != 0;
    int32_t slop = resizable ? RESIZE_SLOP : 0;

    damage_list::rect r = frame_rect(w, w.x, w.y,
                                     static_cast<int32_t>(b->width),
                                     static_cast<int32_t>(b->height));
    if (x < r.x - slop || y < r.y - slop ||
        x >= r.x + r.w + slop || y >= r.y + r.h + slop) {
        return zone::none;
    }

    if (x >= w.x && y >= w.y &&
        x < w.x + static_cast<int32_t>(b->width) &&
        y < w.y + static_cast<int32_t>(b->height)) {
        return zone::content;
    }

    /* The frame band around content and title, and the slop ring just
     * outside it, resize when the window allows it */
    if (resizable) {
        bool band_l = x < w.x;
        bool band_r = x >= w.x + static_cast<int32_t>(b->width);
        bool band_t = y < r.y + BORDER;
        bool band_b = y >= w.y + static_cast<int32_t>(b->height);
        bool near_l = x < r.x + CORNER_REACH;
        bool near_r = x >= r.x + r.w - CORNER_REACH;
        bool near_t = y < r.y + CORNER_REACH;
        bool near_b = y >= r.y + r.h - CORNER_REACH;

        if ((band_t && near_l) || (band_l && near_t)) return zone::resize_tl;
        if ((band_t && near_r) || (band_r && near_t)) return zone::resize_tr;
        if ((band_b && near_l) || (band_l && near_b)) return zone::resize_bl;
        if ((band_b && near_r) || (band_r && near_b)) return zone::resize_br;
        if (band_l) return zone::resize_l;
        if (band_r) return zone::resize_r;
        if (band_t) return zone::resize_t;
        if (band_b) return zone::resize_b;
    }

    if (!decorated(w) || y >= w.y) {
        return zone::none;
    }

    int32_t ccx = w.x + static_cast<int32_t>(b->width)
                - CLOSE_MARGIN - CLOSE_R;
    int32_t ccy = w.y - TITLE_H / 2;
    int32_t dx = x - ccx;
    int32_t dy = y - ccy;
    if (dx * dx + dy * dy <= CLOSE_R * CLOSE_R) {
        return zone::close;
    }

    return zone::title;
}

void draw(const canvas& back, const dm_window& w,
          const chrome_state& st, const damage_list::rect& clip) {
    const dm_buffer* b = current_buffer(w);
    if (!b || !decorated(w)) {
        return;
    }

    /* The title has no width limit, so the view is cut to `bounds` too */
    damage_list::rect area = damage_list::intersect(clip, bounds(w));
    stlxgfx_surface_t* view = clip_view(back, area);
    if (!view) {
        return;
    }

    damage_list::rect frame = frame_rect(w, w.x, w.y, static_cast<int32_t>(b->width), static_cast<int32_t>(b->height));
    draw_body(view, area, { frame.x, frame.y, frame.w, frame.h, CORNER_R }, w.y - 1, st);

    /* Title text, vertically centered in the bar */
    int32_t ox = frame.x - area.x;
    int32_t oy = frame.y - area.y;
    uint32_t title_fg = st.focused ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM;
    int32_t cell_h = g_fm.ascent + g_fm.descent;
    int32_t baseline = oy + (TITLE_H - cell_h) / 2 + g_fm.ascent;
    stlxgfx_draw_text(view, g_font, ox + TITLE_INSET, baseline,
                      w.title, strlen(w.title), title_fg);

    if (st.focused) {
        int32_t ccx = ox + frame.w - CLOSE_MARGIN - CLOSE_R - BORDER;
        int32_t ccy = oy + TITLE_H / 2;
        uint32_t cb_bg = st.close_pressed ? CLOSE_BG_PRESS
                       : st.close_hover   ? CLOSE_BG_HOVER
                       :                    CLOSE_BG;
        uint32_t cb_fg = (st.close_hover || st.close_pressed)
                       ? CLOSE_FG_HOVER : CLOSE_FG;
        stlxgfx_fill_circle(view, ccx, ccy,
                            static_cast<uint32_t>(CLOSE_R), cb_bg);

        int32_t xw = stlxgfx_text_width(g_font, "x", 1);
        stlxgfx_draw_text(view, g_font, ccx - xw / 2,
                          ccy - cell_h / 2 + g_fm.ascent, "x", 1, cb_fg);
    }

    stlxgfx_destroy_surface(view);
}

void blit_content(const canvas& back, const dm_window& w,
                  const damage_list::rect& clip) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return;
    }

    int32_t bw = static_cast<int32_t>(b->width);
    int32_t bh = static_cast<int32_t>(b->height);
    int32_t ix0 = clip.x > w.x ? clip.x : w.x;
    int32_t iy0 = clip.y > w.y ? clip.y : w.y;
    int32_t ix1 = clip.x + clip.w < w.x + bw ? clip.x + clip.w : w.x + bw;
    int32_t iy1 = clip.y + clip.h < w.y + bh ? clip.y + clip.h : w.y + bh;
    if (ix0 >= ix1 || iy0 >= iy1) {
        return;
    }

    /* A picture without a mapping shows the content color, leaving the
     * bottom corner squares to the frame as the compose shader's fill does */
    int32_t inner_r = decorated(w) ? CONTENT_R : 0;
    if (!b->pixels) {
        int32_t corner_h = bh < inner_r ? bh : inner_r;
        fill_inside(back, clip, { w.x, w.y, bw, bh - corner_h }, CONTENT_BG);
        fill_inside(back, clip, { w.x + inner_r, w.y + bh - corner_h, bw - 2 * inner_r, corner_h }, CONTENT_BG);
        return;
    }

    stlxgfx_surface_t* src = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(b->pixels), b->width, b->height,
        b->pitch, 32, 16, 8, 0);
    if (!src) {
        return;
    }

    /* Rows above the corner zone, and undecorated windows entirely,
     * are a plain copy */
    int32_t zone_top = w.y + bh - inner_r;
    int32_t plain_end = iy1 < zone_top ? iy1 : zone_top;
    if (iy0 < plain_end) {
        stlxgfx_blit(back.surface, ix0 - back.x, iy0 - back.y, src, ix0 - w.x, iy0 - w.y,
                     static_cast<uint32_t>(ix1 - ix0),
                     static_cast<uint32_t>(plain_end - iy0));
    }

    /* The bottom rows copy their middle span and blend the corner
     * squares by arc coverage, so the frame's rounding shows through */
    ring_geom content = { w.x, w.y, bw, bh, inner_r };
    int32_t mid0 = w.x + inner_r;
    int32_t mid1 = w.x + bw - inner_r;
    for (int32_t y = iy0 > zone_top ? iy0 : zone_top; y < iy1; y++) {
        int32_t m0 = ix0 > mid0 ? ix0 : mid0;
        int32_t m1 = ix1 < mid1 ? ix1 : mid1;
        if (m0 < m1) {
            stlxgfx_blit(back.surface, m0 - back.x, y - back.y, src, m0 - w.x, y - w.y,
                         static_cast<uint32_t>(m1 - m0), 1);
        }

        uint32_t* dst_row = back_row(back.surface, y - back.y);
        const uint32_t* src_row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(b->pixels) + static_cast<uint32_t>(y - w.y) * b->pitch);
        for (int32_t x = ix0; x < ix1; x++) {
            if (x >= mid0 && x < mid1) {
                x = mid1 - 1;
                continue;
            }

            uint32_t a = dist8_coverage(rrect_dist8(content, x, y));
            if (a == 0) {
                continue;
            }

            dst_row[x - back.x] = blend_px(dst_row[x - back.x], src_row[x - w.x], a);
        }
    }

    stlxgfx_destroy_surface(src);
}

void draw_outline(const canvas& back, const damage_list::rect& r,
                  const damage_list::rect& clip) {
    /* A fill outside the composed rect would cover the pointer that an
     * earlier frame or rect drew over the outline there */
    stlxgfx_surface_t* view = clip_view(back, clip);
    if (!view) {
        return;
    }

    int32_t x = r.x - clip.x;
    int32_t y = r.y - clip.y;
    stlxgfx_fill_rect(view, x, y,
                      static_cast<uint32_t>(r.w), static_cast<uint32_t>(OUTLINE_T), OUTLINE_COLOR);
    stlxgfx_fill_rect(view, x, y + r.h - OUTLINE_T,
                      static_cast<uint32_t>(r.w), static_cast<uint32_t>(OUTLINE_T), OUTLINE_COLOR);
    stlxgfx_fill_rect(view, x, y,
                      static_cast<uint32_t>(OUTLINE_T), static_cast<uint32_t>(r.h), OUTLINE_COLOR);
    stlxgfx_fill_rect(view, x + r.w - OUTLINE_T, y,
                      static_cast<uint32_t>(OUTLINE_T), static_cast<uint32_t>(r.h), OUTLINE_COLOR);
    stlxgfx_destroy_surface(view);
}

} // namespace decor
