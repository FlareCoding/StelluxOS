#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/font.h>

constexpr uint32_t TITLE_BG_FOCUSED   = 0xFF313244;
constexpr uint32_t TITLE_BG_UNFOCUSED = 0xFF1E1E2E;
constexpr uint32_t TITLE_FG_FOCUSED   = 0xFFBAC2DE;
constexpr uint32_t TITLE_FG_UNFOCUSED = 0xFF585B70;
constexpr uint32_t BORDER_FOCUSED     = 0xFF585B70;
constexpr uint32_t BORDER_UNFOCUSED   = 0xFF313244;
constexpr uint32_t CLOSE_BG           = 0xFF45475A;
constexpr uint32_t CLOSE_BG_HOVER     = 0xFFF38BA8;
constexpr uint32_t CLOSE_FG           = 0xFFBAC2DE;
constexpr uint32_t TITLE_FONT_SIZE    = 13;
constexpr uint32_t OUTLINE_COLOR      = 0xFF89B4FA;

static const dm_buffer* current_buffer(const dm_window& w) {
    if (w.current < 0) {
        return nullptr;
    }

    return &w.buffers[static_cast<size_t>(w.current)];
}

namespace decor {

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

zone hit(const dm_window& w, int32_t x, int32_t y) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return zone::none;
    }

    bool resizable = decorated(w) && (w.flags & SWP_WF_RESIZABLE) != 0;
    int32_t slop = resizable ? RESIZE_SLOP : 0;

    damage_list::rect r = bounds(w);
    if (x < r.x - slop || y < r.y - slop ||
        x >= r.x + r.w + slop || y >= r.y + r.h + slop) {
        return zone::none;
    }

    if (x >= w.x && y >= w.y &&
        x < w.x + static_cast<int32_t>(b->width) && y < w.y + static_cast<int32_t>(b->height)) {
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

    int32_t ccx = w.x + static_cast<int32_t>(b->width) - CLOSE_MARGIN;
    int32_t ccy = w.y - TITLE_H / 2;
    int32_t dx = x - ccx;
    int32_t dy = y - ccy;
    if (dx * dx + dy * dy <= CLOSE_R * CLOSE_R) {
        return zone::close;
    }

    return zone::title;
}

void draw(stlxgfx_surface_t* back, const dm_window& w, bool focused,
          bool close_hover) {
    const dm_buffer* b = current_buffer(w);
    if (!b || !decorated(w)) {
        return;
    }

    uint32_t border = focused ? BORDER_FOCUSED : BORDER_UNFOCUSED;
    uint32_t title_bg = focused ? TITLE_BG_FOCUSED : TITLE_BG_UNFOCUSED;
    uint32_t title_fg = focused ? TITLE_FG_FOCUSED : TITLE_FG_UNFOCUSED;
    int32_t bw = static_cast<int32_t>(b->width);
    int32_t bh = static_cast<int32_t>(b->height);

    /* Border ring around content and title */
    stlxgfx_fill_rect(back, w.x - BORDER, w.y - TITLE_H,
                      static_cast<uint32_t>(bw + 2 * BORDER), static_cast<uint32_t>(BORDER),
                      border);
    stlxgfx_fill_rect(back, w.x - BORDER, w.y + bh,
                      static_cast<uint32_t>(bw + 2 * BORDER), static_cast<uint32_t>(BORDER),
                      border);
    stlxgfx_fill_rect(back, w.x - BORDER, w.y - TITLE_H,
                      static_cast<uint32_t>(BORDER),
                      static_cast<uint32_t>(TITLE_H + bh + BORDER), border);
    stlxgfx_fill_rect(back, w.x + bw, w.y - TITLE_H,
                      static_cast<uint32_t>(BORDER),
                      static_cast<uint32_t>(TITLE_H + bh + BORDER), border);

    /* Title bar with the window name and the close control */
    stlxgfx_fill_rect(back, w.x, w.y - TITLE_H + BORDER,
                      static_cast<uint32_t>(bw), static_cast<uint32_t>(TITLE_H - BORDER),
                      title_bg);
    stlxgfx_draw_text(back, w.x + 10, w.y - TITLE_H + 7,
                      w.title, TITLE_FONT_SIZE, title_fg);

    if (focused) {
        int32_t ccx = w.x + bw - CLOSE_MARGIN;
        int32_t ccy = w.y - TITLE_H / 2;
        stlxgfx_fill_circle(back, ccx, ccy, static_cast<uint32_t>(CLOSE_R),
                            close_hover ? CLOSE_BG_HOVER : CLOSE_BG);

        uint32_t xw = 0;
        uint32_t xh = 0;
        stlxgfx_text_size("x", TITLE_FONT_SIZE, &xw, &xh);
        stlxgfx_draw_text(back, ccx - static_cast<int32_t>(xw) / 2,
                          ccy - static_cast<int32_t>(xh) / 2, "x", TITLE_FONT_SIZE,
                          CLOSE_FG);
    }
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
