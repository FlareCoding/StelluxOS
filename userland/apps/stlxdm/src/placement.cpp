#include "decor.hpp"
#include "placement.hpp"
#include "server.hpp"

#include <stlxui/stlxui.h>

#include <cstdlib>
#include <ctime>

/* The largest distance in pixels on each axis from a title press at which a window that is not normal stays put */
constexpr int32_t DRAG_RESTORE_SLOP = 4;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static bool is_near(int32_t x, int32_t y, int32_t other_x, int32_t other_y, int32_t slop) {
    return abs(x - other_x) <= slop && abs(y - other_y) <= slop;
}

/* The content rect `w` was last promised, or the one it shows */
static damage_list::rect newest_content(const dm_window& w) {
    if (w.target_w != 0) {
        return { w.target_x, w.target_y, static_cast<int32_t>(w.target_w), static_cast<int32_t>(w.target_h) };
    }

    if (w.sent_conf_count > 0) {
        const dm_sent_conf& sent = w.sent_confs[w.sent_conf_count - 1];
        return { sent.x, sent.y, static_cast<int32_t>(sent.w), static_cast<int32_t>(sent.h) };
    }

    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];

    return { w.x, w.y, static_cast<int32_t>(b.width), static_cast<int32_t>(b.height) };
}

static uint32_t clamp_to_hints(int32_t length, uint32_t min_length, uint32_t max_length) {
    uint32_t clamped = length > 0 ? static_cast<uint32_t>(length) : 0;
    if (clamped < min_length) {
        clamped = min_length;
    }

    if (max_length != 0 && clamped > max_length) {
        clamped = max_length;
    }

    return clamped;
}

/* A window already of the asked size with no configure waiting or unacked moves at once, and any other is
 * configured to the asked rect */
void server::place_window(dm_window* w, const damage_list::rect& content) {
    dm_client* owner = window_owner(w);
    if (!owner) {
        return;
    }

    const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
    bool is_settled = w->target_w == 0 && w->sent_conf_count == 0;
    bool is_same_size = static_cast<int32_t>(b.width) == content.w && static_cast<int32_t>(b.height) == content.h;
    if (is_settled && is_same_size) {
        move_window_tree(*owner, w, content.x - w->x, content.y - w->y);
        return;
    }

    w->target_w = static_cast<uint32_t>(content.w);
    w->target_h = static_cast<uint32_t>(content.h);
    w->target_x = content.x;
    w->target_y = content.y;
}

/* A maximized window's frame fills its output's work area within its size hints, and a normal window returns to
 * its floating rect, kept in reach */
damage_list::rect server::state_rect(const dm_window& w) const {
    switch (w.placement.state) {
    case window_state::normal:
        break;
    case window_state::maximized: {
        damage_list::rect work = m_layout.work_area(w.output);
        damage_list::rect frame = decor::frame_rect(w, 0, 0, 0, 0);
        uint32_t width = clamp_to_hints(work.w - frame.w, w.min_w, w.max_w);
        uint32_t height = clamp_to_hints(work.h - frame.h, w.min_h, w.max_h);

        return { work.x - frame.x, work.y - frame.y, static_cast<int32_t>(width), static_cast<int32_t>(height) };
    }
    }

    damage_list::rect floating = w.placement.floating;
    desktop_point corner = { floating.x, floating.y };
    desktop_point move = m_layout.reach_move(w.output, &corner, 1, decor::TITLE_H);

    return { floating.x + move.x, floating.y + move.y, floating.w, floating.h };
}

/* Only a resizable toplevel leaves normal, since it must take the size of
 * the state it enters */
void server::set_window_state(dm_window* w, window_state state) {
    bool is_in_state = state == w->placement.state;
    bool has_displayed_buffer = w->current >= 0;
    bool is_resizable_toplevel = w->parent_id == 0 && (w->flags & SWP_WF_RESIZABLE) != 0;
    if (is_in_state || !has_displayed_buffer || !is_resizable_toplevel) {
        return;
    }

    if (w->placement.state == window_state::normal) {
        w->placement.floating = newest_content(*w);
    }

    w->placement.state = state;
    place_window(w, state_rect(*w));
}

void server::toggle_maximized(dm_window* w) {
    bool is_maximized = w->placement.state == window_state::maximized;
    set_window_state(w, is_maximized ? window_state::normal : window_state::maximized);
}

/* Records a press on `w`'s title bar and returns whether it completes a
 * double click, after which a third press starts a new one */
bool server::record_title_press(dm_window* w, int32_t x, int32_t y) {
    uint64_t now = now_ns();
    bool is_double = w == m_title_press && now - m_title_press_ns <= ui::DOUBLE_CLICK_NS &&
                     is_near(x, y, m_title_press_x, m_title_press_y, ui::DOUBLE_CLICK_SLOP);

    m_title_press = is_double ? nullptr : w;
    m_title_press_ns = now;
    m_title_press_x = x;
    m_title_press_y = y;

    return is_double;
}

/* Returns whether the title drag's motion to (x, y) moves its window. One that is not normal stays put while the
 * pointer is near the press, then turns normal with the pointer at the same share of its width. */
bool server::restore_dragged_window(int32_t x, int32_t y) {
    dm_window* w = m_drag;
    if (w->placement.state == window_state::normal) {
        return true;
    }

    if (is_near(x, y, m_title_press_x, m_title_press_y, DRAG_RESTORE_SLOP)) {
        return false;
    }

    dm_client* owner = window_owner(w);
    if (!owner) {
        return false;
    }

    const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
    damage_list::rect floating = w->placement.floating;
    int32_t across = (x - w->x) * floating.w / static_cast<int32_t>(b.width);

    w->placement.state = window_state::normal;
    move_window_tree(*owner, w, x - across - w->x, 0);
    place_window(w, { w->x, w->y, floating.w, floating.h });
    m_drag_dx = x - w->x;

    return true;
}

/* Places again every toplevel that is not normal after the work areas
 * change, so a maximized window keeps filling its output's work area */
void server::refit_placed_windows() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            bool is_toplevel = w->parent_id == 0;
            bool has_displayed_buffer = w->current >= 0;
            bool is_placed = w->placement.state != window_state::normal;
            if (is_toplevel && has_displayed_buffer && is_placed) {
                place_window(w.get(), state_rect(*w));
            }
        }
    }
}
