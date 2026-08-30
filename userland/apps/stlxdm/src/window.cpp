#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Toplevels cascade from here, expressed as the content origin: the
 * frame's outer corner lands at (60, 48) */
constexpr int32_t CASCADE_X = 60 + decor::BORDER;
constexpr int32_t CASCADE_Y = 48 + decor::TITLE_H;
constexpr int32_t CASCADE_STEP = 32;
constexpr uint32_t CASCADE_WRAP = 10;

static dm_window* find_window(dm_client& c, uint32_t win_id) {
    for (auto& w : c.windows) {
        if (w->win_id == win_id) {
            return w.get();
        }
    }

    return nullptr;
}

static dm_buffer* find_buffer(dm_window& w, uint32_t buf_id) {
    for (auto& b : w.buffers) {
        if (b.buf_id == buf_id) {
            return &b;
        }
    }

    return nullptr;
}

static void unmap_buffer(dm_buffer& b) {
    if (b.pixels) {
        munmap(b.pixels, b.size);
        b.pixels = nullptr;
    }
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void server::handle_create_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_create_window*>(payload);
    if (m->width == 0 || m->height == 0 || find_window(c, m->win_id)) {
        c.dead = true;
        return;
    }

    auto w = std::make_unique<dm_window>();
    w->win_id = m->win_id;
    w->parent_id = m->parent_id;
    w->flags = m->flags;
    w->popup_flags = m->popup_flags;
    memcpy(w->title, m->title, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';

    /* Popups sit relative to their parent, toplevels cascade */
    if (m->parent_id != 0) {
        dm_window* parent = find_window(c, m->parent_id);
        if (!parent) {
            c.dead = true;
            return;
        }

        w->x = parent->x + m->rel_x;
        w->y = parent->y + m->rel_y;
    } else {
        int32_t step = CASCADE_STEP
                     * static_cast<int32_t>(m_window_count % CASCADE_WRAP);
        w->x = CASCADE_X + step;
        w->y = CASCADE_Y + step;
    }

    m_window_count++;
    c.windows.push_back(std::move(w));
}

void server::destroy_window_tree(dm_client& c, uint32_t win_id) {
    /* Children go first, matching the client library's cascade */
    for (size_t i = c.windows.size(); i-- > 0;) {
        if (c.windows[i]->parent_id == win_id) {
            destroy_window_tree(c, c.windows[i]->win_id);
        }
    }

    for (size_t i = 0; i < c.windows.size(); i++) {
        if (c.windows[i]->win_id != win_id) {
            continue;
        }

        dm_window* w = c.windows[i].get();
        scene_damage_window(w);

        forget_window(w);
        for (auto& b : c.windows[i]->buffers) {
            unmap_buffer(b);
        }
        c.windows.erase(c.windows.begin() + static_cast<long>(i));
        return;
    }
}

void server::handle_destroy_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_destroy_window*>(payload);
    destroy_window_tree(c, m->win_id);
}

void server::handle_set_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_set_window*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        return;
    }

    switch (m->field) {
    case SWP_FIELD_TITLE:
        memcpy(w->title, m->title, sizeof(w->title));
        w->title[sizeof(w->title) - 1] = '\0';
        return;
    case SWP_FIELD_CURSOR:
        w->cursor = m->a;
        return;
    case SWP_FIELD_MIN_SIZE:
        w->min_w = m->a;
        w->min_h = m->b;
        return;
    case SWP_FIELD_MAX_SIZE:
        w->max_w = m->a;
        w->max_h = m->b;
        return;
    case SWP_FIELD_FULLSCREEN:
        /* Stored and honored once the fullscreen path lands */
        return;
    default:
        return;
    }
}

void server::handle_attach_buffer(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_attach_buffer*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w || m->width == 0 || m->height == 0 ||
        find_buffer(*w, m->buf_id) ||
        memchr(m->shm_name, '/', sizeof(m->shm_name))) {
        c.dead = true;
        return;
    }

    char name[SWP_SHM_NAME_MAX];
    memcpy(name, m->shm_name, sizeof(name));
    name[sizeof(name) - 1] = '\0';

    char path[SWP_SHM_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/dev/shm/%s", name);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        c.dead = true;
        return;
    }

    size_t size = static_cast<size_t>(m->width) * m->height * 4;
    void* pixels = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    /* The registry entry is unlinked as soon as both sides hold their
     * mappings, otherwise a crashed client pins the pages forever */
    unlink(path);
    if (pixels == MAP_FAILED) {
        c.dead = true;
        return;
    }

    dm_buffer b;
    b.buf_id = m->buf_id;
    b.width = m->width;
    b.height = m->height;
    b.size = size;
    b.pixels = static_cast<uint32_t*>(pixels);
    w->buffers.push_back(b);
}

void server::handle_detach_buffer(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_detach_buffer*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        return;
    }

    for (size_t i = 0; i < w->buffers.size(); i++) {
        if (w->buffers[i].buf_id != m->buf_id) {
            continue;
        }

        /* Detaching the displayed or pending buffer is a client bug */
        if (static_cast<int32_t>(i) == w->current || static_cast<int32_t>(i) == w->pending) {
            c.dead = true;
            return;
        }

        unmap_buffer(w->buffers[i]);
        w->buffers.erase(w->buffers.begin() + static_cast<long>(i));
        if (w->current > static_cast<int32_t>(i)) {
            w->current--;
        }
        if (w->pending > static_cast<int32_t>(i)) {
            w->pending--;
        }
        return;
    }
}

void server::handle_commit(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_commit*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        c.dead = true;
        return;
    }

    dm_buffer* b = find_buffer(*w, m->buf_id);
    if (!b) {
        c.dead = true;
        return;
    }

    /* A commit must arrive at the displayed size or at the size of
     * the configure it acks, anything else is a client bug */
    if (w->current >= 0) {
        const dm_buffer& cur = w->buffers[static_cast<size_t>(w->current)];
        bool ok = b->width == cur.width && b->height == cur.height;
        for (uint32_t i = 0; !ok && i < w->sent_conf_count; i++) {
            const dm_sent_conf& e = w->sent_confs[i];
            ok = e.serial == m->ack_serial &&
                 e.w == b->width && e.h == b->height;
        }
        if (!ok) {
            c.dead = true;
            return;
        }
    }

    /* Acks are protocol state, recorded at receipt. Configures older
     * than the ack are history, the acked one stays for the latch. */
    if (m->ack_serial > w->acked_serial) {
        w->acked_serial = m->ack_serial;
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < w->sent_conf_count; i++) {
        if (w->sent_confs[i].serial >= w->acked_serial) {
            w->sent_confs[keep++] = w->sent_confs[i];
        }
    }
    w->sent_conf_count = keep;

    /* Only the latest commit counts, a superseded one is released
     * without ever reaching the screen */
    if (w->pending >= 0) {
        swp_release rel = { w->win_id, w->buffers[static_cast<size_t>(w->pending)].buf_id };
        send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
    }

    for (size_t i = 0; i < w->buffers.size(); i++) {
        if (&w->buffers[i] == b) {
            w->pending = static_cast<int32_t>(i);
            break;
        }
    }

    w->pending_damage_count =
        m->n_damage <= SWP_COMMIT_MAX_RECTS ? m->n_damage : 0;
    for (uint32_t i = 0; i < w->pending_damage_count; i++) {
        w->pending_damage[i] = m->damage[i];
    }
}

bool server::has_latch_work() const {
    for (const auto& c : m_clients) {
        for (const auto& w : c->windows) {
            if (w->pending >= 0) {
                return true;
            }
        }
    }

    return false;
}

bool server::has_configure_work() const {
    for (const auto& c : m_clients) {
        for (const auto& w : c->windows) {
            if (w->target_w != 0 && w->sent_conf_count < 2) {
                return true;
            }
        }
    }

    return false;
}

/* Sends at most one CONFIGURE per window per tick and none while two
 * are unacked, so a slow client skips straight to the newest size
 * instead of chewing through every intermediate one. */
void server::flush_configures() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->target_w == 0 || w->sent_conf_count >= 2) {
                continue;
            }

            /* The size the client already settles at without a send */
            uint32_t ref_w = 0;
            uint32_t ref_h = 0;
            if (w->sent_conf_count > 0) {
                ref_w = w->sent_confs[w->sent_conf_count - 1].w;
                ref_h = w->sent_confs[w->sent_conf_count - 1].h;
            } else if (w->current >= 0) {
                ref_w = w->buffers[static_cast<size_t>(w->current)].width;
                ref_h = w->buffers[static_cast<size_t>(w->current)].height;
            }

            if (w->target_w == ref_w && w->target_h == ref_h) {
                w->target_w = 0;
                w->target_h = 0;
                continue;
            }

            uint32_t serial = ++w->sent_serial;
            w->sent_confs[w->sent_conf_count++] = {
                serial, w->target_w, w->target_h,
                w->target_x, w->target_y
            };

            swp_configure msg = {
                w->win_id, w->target_w, w->target_h, serial,
                w.get() == m_focus ? SWP_STATE_FOCUSED : 0u
            };
            send_to(*c, SWP_MSG_CONFIGURE, &msg, sizeof(msg));

            w->target_w = 0;
            w->target_h = 0;
        }
    }
}

/* Latch a window's pending commit and translate its damage to screen
 * space: the whole window on map or resize, the commit rects otherwise. */
void server::latch_window(dm_client& c, dm_window& w) {
    const dm_buffer& nb = w.buffers[static_cast<size_t>(w.pending)];
    bool first_map = !w.mapped;
    bool resized = false;

    if (w.current >= 0) {
        const dm_buffer& ob = w.buffers[static_cast<size_t>(w.current)];
        resized = ob.width != nb.width || ob.height != nb.height;
        if (resized) {
            scene_damage_window(&w);
        }

        swp_release rel = { w.win_id, ob.buf_id };
        send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
    }

    /* A resize latch moves the window to the spot its configure
     * promised, so an anchored edge stays visually fixed. The acked
     * entry is consumed here. */
    if (resized) {
        for (uint32_t i = 0; i < w.sent_conf_count; i++) {
            const dm_sent_conf& e = w.sent_confs[i];
            if (e.serial == w.acked_serial &&
                e.w == nb.width && e.h == nb.height) {
                w.x = e.x;
                w.y = e.y;
                break;
            }
        }
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < w.sent_conf_count; i++) {
        if (w.sent_confs[i].serial > w.acked_serial) {
            w.sent_confs[keep++] = w.sent_confs[i];
        }
    }
    w.sent_conf_count = keep;

    if (first_map || resized) {
        damage_list::rect nr = { w.x - decor::BORDER - 1,
                                 w.y - decor::TITLE_H - 1,
                                 static_cast<int32_t>(nb.width) + 2 * decor::BORDER + 2,
                                 static_cast<int32_t>(nb.height) + decor::TITLE_H
                                     + decor::BORDER + 2 };
        if (w.flags & SWP_WF_BORDERLESS) {
            nr = { w.x, w.y, static_cast<int32_t>(nb.width), static_cast<int32_t>(nb.height) };
        }
        m_damage.add(nr.x, nr.y, nr.w, nr.h);
    } else if (w.pending_damage_count == 0) {
        m_damage.add(w.x, w.y, static_cast<int32_t>(nb.width), static_cast<int32_t>(nb.height));
    } else {
        for (uint32_t i = 0; i < w.pending_damage_count; i++) {
            const swp_rect& r = w.pending_damage[i];
            m_damage.add(w.x + r.x, w.y + r.y, r.w, r.h);
        }
    }

    w.current = w.pending;
    w.pending = -1;

    if (first_map) {
        w.mapped = true;
        scene_map(&w);
    }

    swp_frame_done done = { w.win_id, 0, now_ns() };
    send_to(c, SWP_MSG_FRAME_DONE, &done, sizeof(done));
}

/* Compose one screen-space rect: background, then every intersecting
 * window in scene order. */
void server::compose_rect(stlxgfx_surface_t* back,
                          const damage_list::rect& r) {
    if (m_wallpaper) {
        stlxgfx_blit(back, r.x, r.y, m_wallpaper, r.x, r.y,
                     static_cast<uint32_t>(r.w),
                     static_cast<uint32_t>(r.h));
    } else {
        stlxgfx_fill_rect(back, r.x, r.y,
                          static_cast<uint32_t>(r.w),
                          static_cast<uint32_t>(r.h), m_conf->bg_color);
    }

    /* Panels live between the wallpaper and the windows */
    m_panels.compose(back, r);

    for (dm_window* w : m_zorder) {
        if (w->current < 0) {
            continue;
        }

        decor::chrome_state st;
        st.focused = w == m_focus;
        st.dragging = w == m_drag;
        st.close_hover = w == m_close_hover;
        st.close_pressed = w == m_close_press;
        decor::draw(back, *w, st, r);

        dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
        int32_t ix0 = r.x > w->x ? r.x : w->x;
        int32_t iy0 = r.y > w->y ? r.y : w->y;
        int32_t ix1 = r.x + r.w < w->x + static_cast<int32_t>(b.width)
                    ? r.x + r.w : w->x + static_cast<int32_t>(b.width);
        int32_t iy1 = r.y + r.h < w->y + static_cast<int32_t>(b.height)
                    ? r.y + r.h : w->y + static_cast<int32_t>(b.height);
        if (ix0 >= ix1 || iy0 >= iy1) {
            continue;
        }

        stlxgfx_surface_t* src = stlxgfx_surface_from_buffer(
            reinterpret_cast<uint8_t*>(b.pixels), b.width, b.height,
            b.width * 4, 32, 16, 8, 0);
        if (src) {
            stlxgfx_blit(back, ix0, iy0, src,
                         ix0 - w->x, iy0 - w->y,
                         static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0));
            stlxgfx_destroy_surface(src);
        }

        /* The square client blit bleeds over the frame's rounded
         * bottom corners, so they are re-carved anti-aliased */
        decor::carve_bottom_corners(back, *w, st, m_wallpaper,
                                    m_conf->bg_color, r);
    }

    if (m_resize) {
        decor::draw_outline(back, m_outline);
    }

    /* Floating chrome above the windows: the dock tooltip and the
     * power star, then the overlay, then the pointer */
    m_panels.compose_top(back, r);
    m_power.draw_star(back, r);
    m_power.draw_overlay(back, r);

    m_cursor.draw(back, m_cursor_shape, m_cursor_x, m_cursor_y);
}

void server::compose_tick() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->pending >= 0) {
                latch_window(*c, *w);
            }
        }
    }

    flush_configures();
    m_panels.flush(m_damage);

    /* Overlay phases: activation edges and transitions repaint the
     * whole screen, a winding hold repaints the orb boxes */
    m_power.update();
    bool power_active = m_power.active();
    if (power_active != m_power_was_active) {
        m_damage.add_full();
        m_power_was_active = power_active;
    } else if (m_power.transitioning()) {
        m_damage.add_full();
    } else if (m_power.animating()) {
        for (int32_t i = 0; i < 2; i++) {
            damage_list::rect ob = m_power.orb_box(i);
            m_damage.add(ob.x, ob.y, ob.w, ob.h);
        }
    }

    /* Collapse frames fade the cached backdrop under the contracting
     * light, skipping scene composition entirely */
    if (m_power.collapsing()) {
        presenter::target t = m_presenter->acquire();
        stlxgfx_surface_t* back = stlxgfx_surface_from_buffer(
            reinterpret_cast<uint8_t*>(t.pixels), m_presenter->width(),
            m_presenter->height(), t.stride, 32, 16, 8, 0);
        if (back) {
            m_power.draw_collapse(back);
            m_cursor.draw(back, m_cursor_shape, m_cursor_x, m_cursor_y);
            stlxgfx_destroy_surface(back);

            damage_list full;
            full.add_full();
            m_presenter->present(full);
            m_history[m_history_head] = full;
            m_history_head = (m_history_head + 1) % DAMAGE_HISTORY;
        }

        m_damage.clear();
        return;
    }

    if (m_damage.empty()) {
        return;
    }

    presenter::target t = m_presenter->acquire();
    uint32_t sw = m_presenter->width();
    uint32_t sh = m_presenter->height();

    /* A target older than one present is missing frames of content,
     * so their damage is unioned back in. Unknown age repaints fully. */
    damage_list effective = m_damage;
    if (t.age == 0 || t.age > DAMAGE_HISTORY + 1) {
        effective.add_full();
    } else {
        for (uint32_t back = 1; back < t.age; back++) {
            uint32_t idx = (m_history_head + DAMAGE_HISTORY - back)
                         % DAMAGE_HISTORY;
            const damage_list& h = m_history[idx];
            if (h.full()) {
                effective.add_full();
                break;
            }

            for (uint32_t i = 0; i < h.count(); i++) {
                const damage_list::rect& r = h.at(i);
                effective.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    stlxgfx_surface_t* back = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(t.pixels), sw, sh, t.stride, 32,
        16, 8, 0);
    if (!back) {
        m_damage.clear();
        return;
    }

    if (effective.full()) {
        damage_list::rect whole = { 0, 0, static_cast<int32_t>(sw), static_cast<int32_t>(sh) };
        compose_rect(back, whole);
    } else {
        for (uint32_t i = 0; i < effective.count(); i++) {
            damage_list::rect r = effective.at(i);
            if (damage_list::clip(r, static_cast<int32_t>(sw), static_cast<int32_t>(sh))) {
                compose_rect(back, r);
            }
        }
    }

    m_presenter->present(effective);
    stlxgfx_destroy_surface(back);

    /* This frame's new damage joins the history for older targets */
    m_history[m_history_head] = m_damage;
    m_history_head = (m_history_head + 1) % DAMAGE_HISTORY;
    m_damage.clear();

    /* Runs only after the collapse's final dark frame is on screen */
    m_power.run_action();
}
