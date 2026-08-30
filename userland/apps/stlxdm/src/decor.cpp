#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/font.h>

namespace {

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

const dm_buffer* current_buffer(const dm_window& w) {
    if (w.current < 0) {
        return nullptr;
    }

    return &w.buffers[(size_t)w.current];
}

} // namespace

namespace decor {

bool decorated(const dm_window& w) {
    return (w.flags & SWP_WF_BORDERLESS) == 0;
}

damage_list::rect bounds(const dm_window& w) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return {};
    }

    if (!decorated(w)) {
        return { w.x, w.y, (int32_t)b->width, (int32_t)b->height };
    }

    return { w.x - BORDER,
             w.y - TITLE_H,
             (int32_t)b->width + 2 * BORDER,
             (int32_t)b->height + TITLE_H + BORDER };
}

zone hit(const dm_window& w, int32_t x, int32_t y) {
    const dm_buffer* b = current_buffer(w);
    if (!b) {
        return zone::none;
    }

    damage_list::rect r = bounds(w);
    if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) {
        return zone::none;
    }

    if (x >= w.x && y >= w.y &&
        x < w.x + (int32_t)b->width && y < w.y + (int32_t)b->height) {
        return zone::content;
    }

    if (!decorated(w) || y >= w.y) {
        return zone::none;
    }

    int32_t ccx = w.x + (int32_t)b->width - CLOSE_MARGIN;
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
    int32_t bw = (int32_t)b->width;
    int32_t bh = (int32_t)b->height;

    /* Border ring around content and title */
    stlxgfx_fill_rect(back, w.x - BORDER, w.y - TITLE_H,
                      (uint32_t)(bw + 2 * BORDER), (uint32_t)BORDER,
                      border);
    stlxgfx_fill_rect(back, w.x - BORDER, w.y + bh,
                      (uint32_t)(bw + 2 * BORDER), (uint32_t)BORDER,
                      border);
    stlxgfx_fill_rect(back, w.x - BORDER, w.y - TITLE_H,
                      (uint32_t)BORDER,
                      (uint32_t)(TITLE_H + bh + BORDER), border);
    stlxgfx_fill_rect(back, w.x + bw, w.y - TITLE_H,
                      (uint32_t)BORDER,
                      (uint32_t)(TITLE_H + bh + BORDER), border);

    /* Title bar with the window name and the close control */
    stlxgfx_fill_rect(back, w.x, w.y - TITLE_H + BORDER,
                      (uint32_t)bw, (uint32_t)(TITLE_H - BORDER),
                      title_bg);
    stlxgfx_draw_text(back, w.x + 10, w.y - TITLE_H + 7,
                      w.title, TITLE_FONT_SIZE, title_fg);

    if (focused) {
        int32_t ccx = w.x + bw - CLOSE_MARGIN;
        int32_t ccy = w.y - TITLE_H / 2;
        stlxgfx_fill_circle(back, ccx, ccy, (uint32_t)CLOSE_R,
                            close_hover ? CLOSE_BG_HOVER : CLOSE_BG);

        uint32_t xw = 0;
        uint32_t xh = 0;
        stlxgfx_text_size("x", TITLE_FONT_SIZE, &xw, &xh);
        stlxgfx_draw_text(back, ccx - (int32_t)xw / 2,
                          ccy - (int32_t)xh / 2, "x", TITLE_FONT_SIZE,
                          CLOSE_FG);
    }
}

} // namespace decor
