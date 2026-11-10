#include "decor.hpp"
#include "placement.hpp"
#include "server.hpp"
#include "shell/dock.hpp"

#include <stlxui/stlxui.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>

/* The largest distance in pixels on each axis from a title press at which a window that is not normal stays put */
constexpr int32_t DRAG_RESTORE_SLOP = 4;

/* A window resampled into its new rect waits at most this long from the
 * animation's start for its client to commit at the new size */
constexpr uint64_t SETTLE_LIMIT_NS = 1000000000ull;

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

static bool is_same_rect(const damage_list::rect& a, const damage_list::rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static dm_window* topmost_toplevel(const std::vector<dm_window*>& zorder) {
    for (size_t i = zorder.size(); i-- > 0;) {
        if (zorder[i]->parent_id == 0) {
            return zorder[i];
        }
    }

    return nullptr;
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
    damage_list::rect content = state_rect(*w);
    damage_list::rect frame = decor::frame_rect(*w, content.x, content.y, content.w, content.h);
    animate_window(w, window_transition::resize, shown_frame(*w), frame);
    place_window(w, content);
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

bool server::is_hidden(const dm_window& top) const {
    bool animates = std::any_of(m_animations.begin(), m_animations.end(), [&top](const window_animation& a) {
        return a.window == &top;
    });

    return top.placement.minimized_ns != 0 || animates;
}

/* Only a toplevel whose app has a dock launcher minimizes, since each click
 * on that launcher brings back the app's window minimized last */
void server::minimize_window(dm_window* w) {
    dm_client* owner = window_owner(w);
    bool is_shown_toplevel = w->parent_id == 0 && w->current >= 0 && w->placement.minimized_ns == 0;
    if (!owner || !is_shown_toplevel || !has_launcher(*owner)) {
        return;
    }

    damage_list::rect frame = shown_frame(*w);
    w->placement.minimized_ns = now_ns();
    scene_hide(w);

    bool focus_left = m_focus && std::find(m_zorder.begin(), m_zorder.end(), m_focus) == m_zorder.end();
    if (focus_left) {
        set_focus(topmost_toplevel(m_zorder));
    }

    animate_window(w, window_transition::minimize, frame, fit_rect(frame, launcher_tile_of(*owner)));
}

/* A window coming back takes the focus immediately and joins the scene when it reaches its place */
void server::unminimize_window(dm_window* w) {
    dm_client* owner = window_owner(w);
    w->placement.minimized_ns = 0;
    set_focus(w);

    damage_list::rect tile = owner ? launcher_tile_of(*owner) : damage_list::rect{};
    bool animates = false;
    if (tile.w > 0 && w->current >= 0) {
        damage_list::rect frame = shown_frame(*w);
        animates = animate_window(w, window_transition::unminimize, fit_rect(frame, tile), frame);
    }

    if (!animates) {
        forget_animation(w);
        scene_show(w);
    }
}

bool server::has_launcher(const dm_client& c) const {
    return m_panels->launcher_of(c.app_id) >= 0;
}

/* The tile of the dock launcher that starts `c`'s app, empty when none does */
damage_list::rect server::launcher_tile_of(const dm_client& c) const {
    int32_t launcher = m_panels->launcher_of(c.app_id);
    if (launcher < 0) {
        return {};
    }

    return m_panels->launcher_tile(static_cast<uint32_t>(launcher));
}

/* Brings back the window of `pin`'s app minimized last, or returns false when
 * none is minimized */
bool server::unminimize_app_window(const stlxconf_pin_t& pin) {
    dm_window* newest = nullptr;
    for (auto& c : m_clients) {
        if (!launches_app(pin, c->app_id)) {
            continue;
        }

        for (auto& w : c->windows) {
            bool is_newer = !newest || w->placement.minimized_ns > newest->placement.minimized_ns;
            if (w->placement.minimized_ns != 0 && is_newer) {
                newest = w.get();
            }
        }
    }

    if (!newest) {
        return false;
    }

    unminimize_window(newest);

    return true;
}

/* Unminimizes every window whose app has no launcher, since a minimized window comes back only through one */
void server::unminimize_windows_without_launcher() {
    for (auto& c : m_clients) {
        if (has_launcher(*c)) {
            continue;
        }

        for (auto& w : c->windows) {
            if (w->placement.minimized_ns != 0) {
                unminimize_window(w.get());
            }
        }
    }
}

window_animation* server::find_animation(const dm_window* w) {
    for (window_animation& a : m_animations) {
        if (a.window == w) {
            return &a;
        }
    }

    return nullptr;
}

/* Shows `w` resampled out of the scene from `from` to `to`, or from where a running animation shows it. False when
 * `w` has no frame or no displayed buffer, and then its change shows at once. */
bool server::animate_window(
    dm_window* w,
    window_transition transition,
    const damage_list::rect& from,
    const damage_list::rect& to
) {
    if (!decor::decorated(*w) || w->current < 0) {
        return false;
    }

    window_animation* running = find_animation(w);
    if (!running) {
        window_animation started;
        started.window = w;
        started.rect = from;
        started.opacity = transition == window_transition::unminimize ? 0 : decor::CHANNEL_MAX;

        m_animations.push_back(std::move(started));
        running = &m_animations.back();
        scene_hide(w);
        damage_painted(*w, from);
    }

    running->transition = transition;
    running->from = running->rect;
    running->from_opacity = running->opacity;
    running->to = to;
    running->to_opacity = transition == window_transition::minimize ? 0 : decor::CHANNEL_MAX;
    running->start_ns = now_ns();

    return true;
}

/* Past its easing, an animation other than a minimize waits for its window's client to commit at the new size, at
 * most `SETTLE_LIMIT_NS` from its start, since the scene shows the displayed buffer at the window's place */
void server::advance_animations() {
    uint64_t now = now_ns();
    for (size_t i = m_animations.size(); i-- > 0;) {
        window_animation& a = m_animations[i];
        dm_window* w = a.window;
        damage_list::rect was = a.rect;
        uint32_t was_opacity = a.opacity;
        bool has_eased = step_animation(a, now);
        if (!is_same_rect(was, a.rect) || was_opacity != a.opacity) {
            damage_painted(*w, was);
            damage_painted(*w, a.rect);
        }

        bool is_minimizing = a.transition == window_transition::minimize;
        bool is_settled = w->target_w == 0 && w->sent_conf_count == 0;
        bool has_waited = now >= a.start_ns + SETTLE_LIMIT_NS;
        if (!has_eased || !(is_minimizing || is_settled || has_waited)) {
            continue;
        }

        damage_painted(*w, a.rect);
        m_animations.erase(m_animations.begin() + static_cast<long>(i));
        if (!is_minimizing) {
            scene_show(w);

            bool is_focus_shown = std::find(m_zorder.begin(), m_zorder.end(), m_focus) != m_zorder.end();
            if (m_focus != w && is_focus_shown) {
                scene_raise(m_focus);
            }
        }
    }
}

void server::forget_animation(const dm_window* w) {
    for (size_t i = 0; i < m_animations.size(); i++) {
        if (m_animations[i].window != w) {
            continue;
        }

        damage_painted(*w, m_animations[i].rect);
        m_animations.erase(m_animations.begin() + static_cast<long>(i));
        return;
    }
}

void server::damage_painted(const dm_window& w, const damage_list::rect& frame) {
    damage_list::rect painted = painted_rect(w, frame);
    m_damage.add(painted.x, painted.y, painted.w, painted.h);
}

/* Draws the animations above the windows of a CPU compose of `r`, from the
 * frame pictures a GPU compose resamples */
void server::compose_animations(const decor::canvas& canvas, const damage_list::rect& r) {
    std::vector<layer_scaled> pieces;
    for (window_animation& a : m_animations) {
        dm_window* w = a.window;
        damage_list::rect area = damage_list::intersect(painted_rect(*w, a.rect), r);
        if (area.w <= 0 || area.h <= 0) {
            continue;
        }

        bool has_frame = w->frame.update(frame_maker(), *w, chrome_of(*w));
        pieces.clear();
        animation_pieces(a, *w, has_frame ? &w->frame : nullptr, pieces);
        for (const layer_scaled& piece : pieces) {
            draw_scaled(canvas, piece, r);
        }
    }
}

/* Adds every animation to `out`'s scene above its windows, in its pixels.
 * False when a frame picture's memory ran out. */
bool server::add_animations(dm_output& out) {
    for (window_animation& a : m_animations) {
        dm_window* w = a.window;
        if (!w->frame.update(*m_gpu_compositor, *w, chrome_of(*w))) {
            return false;
        }

        layer_window shown;
        shown.import = w->buffers[static_cast<size_t>(w->current)].import.get();
        animation_pieces(a, *w, &w->frame, shown.scaled);
        for (layer_scaled& piece : shown.scaled) {
            piece.bounds.x -= out.place.x;
            piece.bounds.y -= out.place.y;
        }

        out.pictures.scene.windows.push_back(std::move(shown));
    }

    return true;
}
