#include "damage.hpp"

#include <algorithm>

/* Writes the parts of `added` outside all of `holes` to `pieces`, which must hold `damage_list::MAX_RECTS` rects, and
 * returns their count. The count exceeds `room` once the parts left after any hole outgrow `room` rects. */
static uint32_t subtract_all(
    const damage_list::rect& added,
    const damage_list::rect* holes,
    uint32_t hole_count,
    uint32_t room,
    damage_list::rect* pieces
) {
    damage_list::rect outside[damage_list::MAX_RECTS];
    uint32_t piece_count = 1;
    pieces[0] = added;
    for (uint32_t i = 0; i < hole_count && piece_count > 0; i++) {
        uint32_t outside_count = 0;
        for (uint32_t j = 0; j < piece_count; j++) {
            damage_list::rect parts[damage_list::PARTS_OUTSIDE_MAX];
            uint32_t part_count = damage_list::subtract(pieces[j], holes[i], parts);
            if (outside_count + part_count > room) {
                return room + 1;
            }

            std::copy(parts, parts + part_count, outside + outside_count);
            outside_count += part_count;
        }

        std::copy(outside, outside + outside_count, pieces);
        piece_count = outside_count;
    }

    return piece_count;
}

void damage_list::add(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (m_full || w <= 0 || h <= 0) {
        return;
    }

    rect added = { x, y, w, h };
    for (uint32_t i = 0; i < m_count; i++) {
        if (contains(m_rects[i], added)) {
            return;
        }
    }

    uint32_t kept = 0;
    for (uint32_t i = 0; i < m_count; i++) {
        if (!contains(added, m_rects[i])) {
            m_rects[kept++] = m_rects[i];
        }
    }

    m_count = kept;

    rect pieces[MAX_RECTS];
    uint32_t room = MAX_RECTS - m_count;
    uint32_t piece_count = subtract_all(added, m_rects, m_count, room, pieces);
    if (piece_count > room) {
        m_full = true;
        return;
    }

    std::copy(pieces, pieces + piece_count, m_rects + m_count);
    m_count += piece_count;
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

bool damage_list::contains(const rect& outer, const rect& inner) {
    bool holds_columns = inner.x >= outer.x && inner.x + inner.w <= outer.x + outer.w;
    bool holds_rows = inner.y >= outer.y && inner.y + inner.h <= outer.y + outer.h;

    return holds_columns && holds_rows;
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

uint32_t damage_list::subtract(const rect& piece, const rect& hole, rect* parts) {
    rect shared = intersect(piece, hole);
    if (shared.w <= 0 || shared.h <= 0) {
        parts[0] = piece;
        return 1;
    }

    int32_t piece_right = piece.x + piece.w;
    int32_t piece_bottom = piece.y + piece.h;
    int32_t shared_right = shared.x + shared.w;
    int32_t shared_bottom = shared.y + shared.h;

    uint32_t count = 0;
    if (shared.y > piece.y) {
        parts[count++] = { piece.x, piece.y, piece.w, shared.y - piece.y };
    }

    if (shared_bottom < piece_bottom) {
        parts[count++] = { piece.x, shared_bottom, piece.w, piece_bottom - shared_bottom };
    }

    if (shared.x > piece.x) {
        parts[count++] = { piece.x, shared.y, shared.x - piece.x, shared.h };
    }

    if (shared_right < piece_right) {
        parts[count++] = { shared_right, shared.y, piece_right - shared_right, shared.h };
    }

    return count;
}
