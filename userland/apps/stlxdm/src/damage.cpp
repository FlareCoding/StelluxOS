#include "damage.hpp"

#include <algorithm>

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

damage_list::rect damage_list::intersect(const rect& a, const rect& b) {
    int32_t left = std::max(a.x, b.x);
    int32_t top = std::max(a.y, b.y);
    int32_t right = std::min(a.x + a.w, b.x + b.w);
    int32_t bottom = std::min(a.y + a.h, b.y + b.h);
    if (right <= left || bottom <= top) {
        return {};
    }

    return { left, top, right - left, bottom - top };
}

damage_list::rect damage_list::union_of(const rect& a, const rect& b) {
    if (a.w <= 0 || a.h <= 0) {
        return b;
    }

    if (b.w <= 0 || b.h <= 0) {
        return a;
    }

    int32_t left = std::min(a.x, b.x);
    int32_t top = std::min(a.y, b.y);
    int32_t right = std::max(a.x + a.w, b.x + b.w);
    int32_t bottom = std::max(a.y + a.h, b.y + b.h);

    return { left, top, right - left, bottom - top };
}
