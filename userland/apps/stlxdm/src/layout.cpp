#include "layout.hpp"

#include <algorithm>

static int64_t rect_area(const damage_list::rect& r) {
    return static_cast<int64_t>(r.w) * r.h;
}

static damage_list::rect intersect(const damage_list::rect& a, const damage_list::rect& b) {
    int32_t left = std::max(a.x, b.x);
    int32_t top = std::max(a.y, b.y);
    int32_t right = std::min(a.x + a.w, b.x + b.w);
    int32_t bottom = std::min(a.y + a.h, b.y + b.h);
    if (right <= left || bottom <= top) {
        return {};
    }

    return { left, top, right - left, bottom - top };
}

static bool holds(const damage_list::rect& r, int64_t x, int64_t y) {
    return x >= r.x && x < static_cast<int64_t>(r.x) + r.w && y >= r.y && y < static_cast<int64_t>(r.y) + r.h;
}

/* Whether `a` and `b` touch along an edge at least one pixel long */
static bool share_edge(const damage_list::rect& a, const damage_list::rect& b) {
    bool rows_meet = std::min(a.y + a.h, b.y + b.h) > std::max(a.y, b.y);
    bool columns_meet = std::min(a.x + a.w, b.x + b.w) > std::max(a.x, b.x);
    bool side_by_side = a.x + a.w == b.x || b.x + b.w == a.x;
    bool stacked = a.y + a.h == b.y || b.y + b.h == a.y;

    return (side_by_side && rows_meet) || (stacked && columns_meet);
}

static bool within_limit(int64_t coordinate) {
    return coordinate >= -layout::DESKTOP_LIMIT && coordinate <= layout::DESKTOP_LIMIT;
}

/* The least shift bringing every position from `smallest` to `largest` into `low` to `high`, or the least bringing
 * in `largest` alone when they spread wider */
static int32_t least_shift(int32_t smallest, int32_t largest, int32_t low, int32_t high) {
    high = std::max(high, low);

    int32_t least = low - smallest;
    int32_t most = high - largest;
    if (least <= most) {
        return std::clamp(0, least, most);
    }

    return std::clamp(0, low - largest, high - largest);
}

/* How far a segment going `length` along one axis and `other_length` along the other has gone along the other as it
 * crosses the edge `room` pixels away on the first, rounded to the nearer pixel and a half rounded back */
static int64_t offset_at_crossing(int64_t room, int64_t length, int64_t other_length) {
    int64_t gone = other_length * (2 * room + 1);
    int64_t span = 2 * length;
    return (2 * gone + span - 1) / (2 * span);
}

bool layout::rebuild(const member* members, uint32_t count, uint32_t primary) {
    if (count == 0) {
        return false;
    }

    m_main = choose_main(members, count, primary);
    m_count = count;

    int32_t next_x = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (members[i].output == m_main) {
            next_x = members[i].width;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        const member& m = members[i];
        m_places[i].output = m.output;
        m_places[i].rect = { 0, 0, m.width, m.height };
        if (m.output == m_main) {
            continue;
        }

        m_places[i].rect.x = next_x;
        next_x += m.width;
    }

    return true;
}

uint32_t layout::choose_main(const member* members, uint32_t count, uint32_t primary) const {
    bool main_stays = false;
    bool primary_joins = false;
    for (uint32_t i = 0; i < count; i++) {
        main_stays = main_stays || (m_count != 0 && members[i].output == m_main);
        primary_joins = primary_joins || members[i].output == primary;
    }

    if (main_stays) {
        return m_main;
    }

    return primary_joins ? primary : members[0].output;
}

const layout::place* layout::find(uint32_t output) const {
    for (uint32_t i = 0; i < m_count; i++) {
        if (m_places[i].output == output) {
            return &m_places[i];
        }
    }

    return nullptr;
}

int32_t layout::index_at(int64_t x, int64_t y) const {
    for (uint32_t i = 0; i < m_count; i++) {
        if (holds(m_places[i].rect, x, y)) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

int32_t layout::output_at(int64_t x, int64_t y) const {
    int32_t index = index_at(x, y);
    return index < 0 ? -1 : static_cast<int32_t>(m_places[index].output);
}

bool layout::is_valid() const {
    const place* main = find(m_main);
    if (!main || main->rect.x != 0 || main->rect.y != 0) {
        return false;
    }

    for (uint32_t i = 0; i < m_count; i++) {
        const damage_list::rect& r = m_places[i].rect;
        int64_t right = static_cast<int64_t>(r.x) + r.w;
        int64_t bottom = static_cast<int64_t>(r.y) + r.h;
        if (!within_limit(r.x) || !within_limit(r.y) || !within_limit(right) || !within_limit(bottom)) {
            return false;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (rect_area(intersect(r, m_places[j].rect)) != 0) {
                return false;
            }
        }
    }

    bool reached[MEMBERS_MAX] = {};
    reached[main - m_places] = true;
    for (bool spread = true; spread;) {
        spread = false;
        for (uint32_t i = 0; i < m_count; i++) {
            for (uint32_t j = 0; j < m_count; j++) {
                if (reached[i] && !reached[j] && share_edge(m_places[i].rect, m_places[j].rect)) {
                    reached[j] = true;
                    spread = true;
                }
            }
        }
    }

    return std::all_of(reached, reached + m_count, [](bool r) { return r; });
}

void layout::set_shell_rows(int32_t top_rows, int32_t bottom_y) {
    m_shell_top_rows = top_rows;
    m_shell_bottom_y = bottom_y;
}

damage_list::rect layout::work_area(uint32_t output) const {
    const place* p = find(output);
    if (!p) {
        return {};
    }

    if (output != m_main) {
        return p->rect;
    }

    int32_t top = std::clamp(m_shell_top_rows, 0, p->rect.h);
    int32_t bottom = std::clamp(m_shell_bottom_y, top, p->rect.h);

    return { p->rect.x, p->rect.y + top, p->rect.w, bottom - top };
}

bool layout::covers(const damage_list::rect& r) const {
    if (r.w <= 0 || r.h <= 0) {
        return true;
    }

    /* Work areas lie inside places, which never overlap, so their
     * shares of `r` add up without counting a pixel twice */
    int64_t covered = 0;
    for (uint32_t i = 0; i < m_count; i++) {
        covered += rect_area(intersect(r, work_area(m_places[i].output)));
    }

    return covered == rect_area(r);
}

int32_t layout::output_holding(const damage_list::rect& frame, const layout* among) const {
    int32_t best = -1;
    int64_t best_area = 0;
    for (uint32_t i = 0; i < m_count; i++) {
        if (among && !among->find(m_places[i].output)) {
            continue;
        }

        int64_t area = rect_area(intersect(frame, m_places[i].rect));
        if (area > best_area) {
            best = static_cast<int32_t>(m_places[i].output);
            best_area = area;
        }
    }

    return best;
}

desktop_point layout::nearest_point(desktop_point point) const {
    desktop_point best = point;
    uint64_t best_distance = UINT64_MAX;
    for (uint32_t i = 0; i < m_count; i++) {
        const damage_list::rect& r = m_places[i].rect;
        int32_t x = std::clamp(point.x, r.x, r.x + r.w - 1);
        int32_t y = std::clamp(point.y, r.y, r.y + r.h - 1);

        int64_t dx = static_cast<int64_t>(x) - point.x;
        int64_t dy = static_cast<int64_t>(y) - point.y;
        uint64_t distance = static_cast<uint64_t>(dx * dx) + static_cast<uint64_t>(dy * dy);
        if (distance < best_distance) {
            best = { x, y };
            best_distance = distance;
        }
    }

    return best;
}

desktop_point layout::move_pointer(desktop_point from, int32_t dx, int32_t dy) const {
    int32_t index = index_at(from.x, from.y);
    if (index < 0) {
        return nearest_point(from);
    }

    int64_t x = from.x;
    int64_t y = from.y;
    int64_t target_x = x + dx;
    int64_t target_y = y + dy;

    /* Every pass enters another place or spends the motion along one
     * axis, and a segment never enters a place twice */
    for (uint32_t pass = 0; pass < MEMBERS_MAX + 3; pass++) {
        const damage_list::rect& r = m_places[index].rect;
        int64_t left = r.x;
        int64_t top = r.y;
        int64_t right = left + r.w - 1;
        int64_t bottom = top + r.h - 1;

        int64_t rest_x = target_x - x;
        int64_t rest_y = target_y - y;
        int64_t room_x = rest_x > 0 ? right - x : x - left;
        int64_t room_y = rest_y > 0 ? bottom - y : y - top;

        int64_t length_x = rest_x < 0 ? -rest_x : rest_x;
        int64_t length_y = rest_y < 0 ? -rest_y : rest_y;
        bool leaves_x = length_x > room_x;
        bool leaves_y = length_y > room_y;
        if (!leaves_x && !leaves_y) {
            x = target_x;
            y = target_y;
            break;
        }

        /* The segment crosses first the edge it meets sooner, the column edge when it meets both at a corner, and
         * always within the place's span along the other axis */
        bool x_first = leaves_x && (!leaves_y || (2 * room_x + 1) * length_y <= (2 * room_y + 1) * length_x);
        if (x_first) {
            int64_t offset = offset_at_crossing(room_x, length_x, length_y);
            int64_t cross_x = rest_x > 0 ? right + 1 : left - 1;
            int64_t cross_y = rest_y < 0 ? y - offset : y + offset;
            int32_t next = index_at(cross_x, cross_y);
            if (next >= 0) {
                index = next;
                x = cross_x;
                y = cross_y;
                continue;
            }

            x = rest_x > 0 ? right : left;
            y = cross_y;
            target_x = x;
            continue;
        }

        int64_t offset = offset_at_crossing(room_y, length_y, length_x);
        int64_t cross_y = rest_y > 0 ? bottom + 1 : top - 1;
        int64_t cross_x = rest_x < 0 ? x - offset : x + offset;
        int32_t next = index_at(cross_x, cross_y);
        if (next >= 0) {
            index = next;
            x = cross_x;
            y = cross_y;
            continue;
        }

        y = rest_y > 0 ? bottom : top;
        x = cross_x;
        target_y = y;
    }

    return { static_cast<int32_t>(x), static_cast<int32_t>(y) };
}

desktop_point layout::follow_point(desktop_point point, const layout& before) const {
    int32_t output = before.output_at(point.x, point.y);
    const place* now = output < 0 ? nullptr : find(static_cast<uint32_t>(output));
    if (!now) {
        const place* main = find(m_main);
        if (!main) {
            return point;
        }

        return { main->rect.x + main->rect.w / 2, main->rect.y + main->rect.h / 2 };
    }

    const place* then = before.find(static_cast<uint32_t>(output));
    int32_t x = point.x - then->rect.x + now->rect.x;
    int32_t y = point.y - then->rect.y + now->rect.y;

    return { std::clamp(x, now->rect.x, now->rect.x + now->rect.w - 1),
             std::clamp(y, now->rect.y, now->rect.y + now->rect.h - 1) };
}

desktop_point layout::reach_move(
    uint32_t output,
    const desktop_point* corners,
    uint32_t count,
    int32_t grip_size
) const {
    if (count == 0) {
        return {};
    }

    bool covered = true;
    desktop_point smallest = corners[0];
    desktop_point largest = corners[0];
    for (uint32_t i = 0; i < count; i++) {
        covered = covered && covers({ corners[i].x, corners[i].y - grip_size, grip_size, grip_size });
        smallest = { std::min(smallest.x, corners[i].x), std::min(smallest.y, corners[i].y) };
        largest = { std::max(largest.x, corners[i].x), std::max(largest.y, corners[i].y) };
    }

    damage_list::rect work = work_area(output);
    if (covered || work.w <= 0 || work.h <= 0) {
        return {};
    }

    int32_t move_x = least_shift(smallest.x, largest.x, work.x, work.x + work.w - grip_size);
    int32_t move_y = least_shift(smallest.y, largest.y, work.y + grip_size, work.y + work.h);

    return { move_x, move_y };
}
