#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>

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

/* The chrome face at the title size, opened once at startup */
static stlxgfx_font* g_font = nullptr;
static stlxgfx_font_metrics g_fm = {};

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

namespace decor {

int init() {
    g_font = stlxgfx_font_open(STLXGFX_FONT_PATH, TITLE_FONT_SIZE);
    if (!g_font) {
        return -1;
    }

    stlxgfx_font_metrics_get(g_font, &g_fm);
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

void carve_bottom_corners(stlxgfx_surface_t* back, const dm_window& w,
                          const chrome_state& st,
                          const stlxgfx_surface_t* wallpaper,
                          uint32_t bg_color,
                          const damage_list::rect& clip) {
    const dm_buffer* b = current_buffer(w);
    if (!b || !decorated(w)) {
        return;
    }

    stlxgfx_surface_t* view = clip_view(back, clip);
    if (!view) {
        return;
    }

    /* The arcs anchor on the border ring's outer corners */
    int32_t left = w.x - BORDER - clip.x;
    int32_t right = left + static_cast<int32_t>(b->width) + 2 * BORDER;
    int32_t bottom = w.y - TITLE_H - clip.y
                   + static_cast<int32_t>(b->height) + TITLE_H + BORDER;
    uint32_t border = border_color(st);
    uint32_t inner_r = CORNER_R - BORDER;

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, view);

    /* Restore the background outside the rounded corner, sampling the
     * wallpaper through a view aligned with this one */
    if (wallpaper) {
        stlxgfx_surface_t* wp_view = stlxgfx_surface_from_buffer(
            wallpaper->pixels
                + static_cast<uint32_t>(clip.y) * wallpaper->pitch
                + static_cast<uint32_t>(clip.x) * 4,
            static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
            wallpaper->pitch, 32, 16, 8, 0);
        if (wp_view) {
            stlxgfx_blit_arc_corner(view, left, bottom, CORNER_R, 0,
                                    1, -1, 1, wp_view);
            stlxgfx_blit_arc_corner(view, right, bottom, CORNER_R, 0,
                                    -1, -1, 1, wp_view);
            stlxgfx_destroy_surface(wp_view);
        }
    } else {
        stlxgfx_ctx_fill_arc_corner(&ctx, left, bottom, CORNER_R, 0,
                                    1, -1, 1, bg_color);
        stlxgfx_ctx_fill_arc_corner(&ctx, right, bottom, CORNER_R, 0,
                                    -1, -1, 1, bg_color);
    }

    stlxgfx_ctx_fill_arc_corner(&ctx, left, bottom, CORNER_R, inner_r,
                                1, -1, 0, border);
    stlxgfx_ctx_fill_arc_corner(&ctx, right, bottom, CORNER_R, inner_r,
                                -1, -1, 0, border);

    stlxgfx_destroy_surface(view);
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
