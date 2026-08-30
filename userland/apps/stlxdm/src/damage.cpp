#include "damage.hpp"

void damage_list::add(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (m_full || w <= 0 || h <= 0) {
        return;
    }

    if (m_count == MAX_RECTS) {
        m_full = true;
        return;
    }

    m_rects[m_count++] = { x, y, w, h };
}

void damage_list::clear() {
    m_count = 0;
    m_full = false;
}

bool damage_list::clip(rect& r, int32_t screen_w, int32_t screen_h) {
    int32_t x1 = r.x + r.w;
    int32_t y1 = r.y + r.h;

    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (x1 > screen_w) x1 = screen_w;
    if (y1 > screen_h) y1 = screen_h;

    r.w = x1 - r.x;
    r.h = y1 - r.y;
    return r.w > 0 && r.h > 0;
}
