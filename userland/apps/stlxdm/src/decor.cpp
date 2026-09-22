#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/blend.h>
#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>

#include <cmath>
#include <cstring>

constexpr uint32_t TITLE_BG_FOCUSED   = 0xFF313244;
constexpr uint32_t TITLE_BG_UNFOCUSED = 0xFF1E1E2E;
constexpr uint32_t TITLE_FG_FOCUSED   = 0xFFBAC2DE;
constexpr uint32_t TITLE_FG_UNFOCUSED = 0xFF585B70;
constexpr uint32_t BORDER_FOCUSED     = 0xFF585B70;
constexpr uint32_t BORDER_DRAGGING    = 0xFF89B4FA;
constexpr uint32_t BORDER_UNFOCUSED   = 0xFF313244;
constexpr uint32_t CONTENT_BG         = 0xFF2D2D30;
constexpr uint32_t CLOSE_BG           = 0xFF45475A;
constexpr uint32_t CLOSE_BG_HOVER     = 0xFFF38BA8;
constexpr uint32_t CLOSE_BG_PRESS     = 0xFFD06080;
constexpr uint32_t CLOSE_FG           = 0xFFBAC2DE;
constexpr uint32_t CLOSE_FG_HOVER     = 0xFFFFFFFF;
constexpr uint32_t TITLE_FONT_SIZE    = 13;
constexpr uint32_t OUTLINE_COLOR      = 0xFF89B4FA;

/* Corner distances run through a table of squared half pixel offsets,
 * so the content arcs never need a square root per pixel */
constexpr int32_t SDF_Q_MAX = 2 * decor::CORNER_R;
constexpr int32_t SDF_LUT_N = 2 * SDF_Q_MAX * SDF_Q_MAX + 1;

/* A rounded rect in pixels, the shape the content corners follow */
struct ring_geom {
    int32_t x = 0, y = 0;
    int32_t w = 0, h = 0;
    int32_t r = 0;
};

/* The chrome face at the title size, opened once at startup */
static stlxgfx_font* g_font = nullptr;
static stlxgfx_font_metrics g_fm = {};
static uint16_t g_dist8[SDF_LUT_N];

static const dm_buffer* current_buffer(const dm_window& w) {
    if (w.current < 0) {
        return nullptr;
    }

    return &w.buffers[static_cast<size_t>(w.current)];
}

static uint32_t border_color(const decor::chrome_state& st) {
    if (st.dragging) {
        return BORDER_DRAGGING;
    }

    return st.focused ? BORDER_FOCUSED : BORDER_UNFOCUSED;
}

/* A writable view over the clip rect, so every primitive clips for
 * free. Coordinates shift into view space at the call sites. */
static stlxgfx_surface_t* clip_view(stlxgfx_surface_t* back,
                                    const damage_list::rect& clip) {
    if (clip.w <= 0 || clip.h <= 0) {
        return nullptr;
    }

    return stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(clip.y) * back->pitch
            + static_cast<uint32_t>(clip.x) * 4,
        static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
        back->pitch, 32, 16, 8, 0);
}

static void sdf_table_init() {
    for (int32_t k = 0; k < SDF_LUT_N; k++) {
        g_dist8[k] = static_cast<uint16_t>(sqrtf(static_cast<float>(k)) * 4.0f + 0.5f);
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

namespace decor {

int init() {
    g_font = stlxgfx_font_open(STLXGFX_UI_FONT_MEDIUM_PATH, TITLE_FONT_SIZE);
    if (!g_font) {
        return -1;
    }

    stlxgfx_font_metrics_get(g_font, &g_fm);
    sdf_table_init();
    return 0;
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

    damage_list::rect r = frame_rect(w, w.x, w.y,
                                     static_cast<int32_t>(b->width),
                                     static_cast<int32_t>(b->height));

    /* One extra pixel on each side covers the drag glow ring */
    if (decorated(w)) {
        r = { r.x - 1, r.y - 1, r.w + 2, r.h + 2 };
    }

    return r;
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

void draw(stlxgfx_surface_t* back, const dm_window& w,
          const chrome_state& st, const damage_list::rect& clip) {
    const dm_buffer* b = current_buffer(w);
    if (!b || !decorated(w)) {
        return;
    }

    stlxgfx_surface_t* view = clip_view(back, clip);
    if (!view) {
        return;
    }

    /* Outer geometry in view space, the border ring's own origin */
    int32_t ox = w.x - BORDER - clip.x;
    int32_t oy = w.y - TITLE_H - clip.y;
    uint32_t outer_w = b->width + 2 * BORDER;
    uint32_t outer_h = b->height + TITLE_H + BORDER;
    uint32_t border = border_color(st);
    uint32_t title_bg = st.focused ? TITLE_BG_FOCUSED : TITLE_BG_UNFOCUSED;

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, view);

    /* The drag glow is a one pixel halo behind the rounded border */
    if (st.dragging) {
        stlxgfx_ctx_fill_rounded_rect(&ctx, ox - 1, oy - 1,
                                      outer_w + 2, outer_h + 2,
                                      CORNER_R + 1, BORDER_DRAGGING);
    }

    stlxgfx_ctx_fill_rounded_rect(&ctx, ox, oy, outer_w, outer_h,
                                  CORNER_R, border);

    uint32_t inner_r = CORNER_R - BORDER;

    /* Title bar: rounded top corners, squared off at its bottom */
    stlxgfx_ctx_fill_rounded_rect(&ctx, ox + BORDER, oy + BORDER,
                                  outer_w - 2 * BORDER,
                                  TITLE_H - BORDER, inner_r, title_bg);
    stlxgfx_ctx_fill_rect(&ctx, ox + BORDER,
                          oy + TITLE_H - static_cast<int32_t>(inner_r),
                          outer_w - 2 * BORDER, inner_r, title_bg);

    /* Content backdrop under the client blit: flat top, rounded
     * bottom corners */
    uint32_t content_w = outer_w - 2 * BORDER;
    uint32_t content_h = outer_h - TITLE_H - BORDER;
    int32_t cx = ox + BORDER;
    int32_t cy = oy + TITLE_H;

    if (content_h > inner_r) {
        int32_t cb = cy + static_cast<int32_t>(content_h);
        int32_t cr = cx + static_cast<int32_t>(content_w);
        stlxgfx_ctx_fill_rect(&ctx, cx, cy, content_w,
                              content_h - inner_r, CONTENT_BG);
        stlxgfx_ctx_fill_rect(&ctx, cx + static_cast<int32_t>(inner_r),
                              cb - static_cast<int32_t>(inner_r),
                              content_w - 2 * inner_r, inner_r, CONTENT_BG);
        stlxgfx_ctx_fill_arc_corner(&ctx, cx, cb, inner_r, 0,
                                    1, -1, 0, CONTENT_BG);
        stlxgfx_ctx_fill_arc_corner(&ctx, cr, cb, inner_r, 0,
                                    -1, -1, 0, CONTENT_BG);
    } else {
        stlxgfx_ctx_fill_rect(&ctx, cx, cy, content_w, content_h,
                              CONTENT_BG);
    }

    /* Title and content separator line */
    stlxgfx_ctx_fill_rect(&ctx, ox + BORDER, oy + TITLE_H - 1,
                          outer_w - 2 * BORDER, 1, border);

    /* Title text, vertically centered in the bar */
    uint32_t title_fg = st.focused ? TITLE_FG_FOCUSED : TITLE_FG_UNFOCUSED;
    int32_t cell_h = g_fm.ascent + g_fm.descent;
    int32_t baseline = oy + (TITLE_H - cell_h) / 2 + g_fm.ascent;
    stlxgfx_draw_text(view, g_font, ox + 12, baseline,
                      w.title, strlen(w.title), title_fg);

    if (st.focused) {
        int32_t ccx = ox + static_cast<int32_t>(outer_w)
                    - CLOSE_MARGIN - CLOSE_R - BORDER;
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

void blit_content(stlxgfx_surface_t* back, const dm_window& w,
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

    stlxgfx_surface_t* src = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(b->pixels), b->width, b->height,
        b->width * 4, 32, 16, 8, 0);
    if (!src) {
        return;
    }

    /* Rows above the corner zone, and undecorated windows entirely,
     * are a plain copy */
    int32_t inner_r = decorated(w) ? CONTENT_R : 0;
    int32_t zone_top = w.y + bh - inner_r;
    int32_t plain_end = iy1 < zone_top ? iy1 : zone_top;
    if (iy0 < plain_end) {
        stlxgfx_blit(back, ix0, iy0, src, ix0 - w.x, iy0 - w.y,
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
            stlxgfx_blit(back, m0, y, src, m0 - w.x, y - w.y,
                         static_cast<uint32_t>(m1 - m0), 1);
        }

        uint32_t* dst_row = back_row(back, y);
        const uint32_t* src_row = b->pixels + static_cast<uint32_t>(y - w.y) * b->width;
        for (int32_t x = ix0; x < ix1; x++) {
            if (x >= mid0 && x < mid1) {
                x = mid1 - 1;
                continue;
            }

            uint32_t a = dist8_coverage(rrect_dist8(content, x, y));
            if (a == 0) {
                continue;
            }
            dst_row[x] = blend_px(dst_row[x], src_row[x - w.x], a);
        }
    }

    stlxgfx_destroy_surface(src);
}

void draw_outline(stlxgfx_surface_t* back, const damage_list::rect& r) {
    stlxgfx_fill_rect(back, r.x, r.y,
                      static_cast<uint32_t>(r.w), static_cast<uint32_t>(OUTLINE_T), OUTLINE_COLOR);
    stlxgfx_fill_rect(back, r.x, r.y + r.h - OUTLINE_T,
                      static_cast<uint32_t>(r.w), static_cast<uint32_t>(OUTLINE_T), OUTLINE_COLOR);
    stlxgfx_fill_rect(back, r.x, r.y,
                      static_cast<uint32_t>(OUTLINE_T), static_cast<uint32_t>(r.h), OUTLINE_COLOR);
    stlxgfx_fill_rect(back, r.x + r.w - OUTLINE_T, r.y,
                      static_cast<uint32_t>(OUTLINE_T), static_cast<uint32_t>(r.h), OUTLINE_COLOR);
}

} // namespace decor
