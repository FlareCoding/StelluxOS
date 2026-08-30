#include "decor.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr int32_t CASCADE_ORIGIN = 60;
constexpr int32_t CASCADE_STEP = 40;
constexpr uint32_t BACKGROUND = 0xFF16161E;

dm_window* find_window(dm_client& c, uint32_t win_id) {
    for (auto& w : c.windows) {
        if (w->win_id == win_id) {
            return w.get();
        }
    }

    return nullptr;
}

dm_buffer* find_buffer(dm_window& w, uint32_t buf_id) {
    for (auto& b : w.buffers) {
        if (b.buf_id == buf_id) {
            return &b;
        }
    }

    return nullptr;
}

void unmap_buffer(dm_buffer& b) {
    if (b.pixels) {
        munmap(b.pixels, b.size);
        b.pixels = nullptr;
    }
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

} // namespace

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
        int32_t step = CASCADE_ORIGIN
                     + CASCADE_STEP * (int32_t)(m_window_count % 8);
        w->x = step;
        w->y = step;
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
        c.windows.erase(c.windows.begin() + (long)i);
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
    case SWP_FIELD_MAX_SIZE:
    case SWP_FIELD_FULLSCREEN:
        /* Stored and honored once interactive resize lands */
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

    size_t size = (size_t)m->width * m->height * 4;
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
        if ((int32_t)i == w->current || (int32_t)i == w->pending) {
            c.dead = true;
            return;
        }

        unmap_buffer(w->buffers[i]);
        w->buffers.erase(w->buffers.begin() + (long)i);
        if (w->current > (int32_t)i) {
            w->current--;
        }
        if (w->pending > (int32_t)i) {
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

    /* Acks are protocol state, recorded at receipt */
    if (m->ack_serial > w->acked_serial) {
        w->acked_serial = m->ack_serial;
    }

    /* Only the latest commit counts, a superseded one is released
     * without ever reaching the screen */
    if (w->pending >= 0) {
        swp_release rel = { w->win_id, w->buffers[(size_t)w->pending].buf_id };
        send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
    }

    for (size_t i = 0; i < w->buffers.size(); i++) {
        if (&w->buffers[i] == b) {
            w->pending = (int32_t)i;
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

/* Latch a window's pending commit and translate its damage to screen
 * space: the whole window on map or resize, the commit rects otherwise. */
void server::latch_window(dm_client& c, dm_window& w) {
    const dm_buffer& nb = w.buffers[(size_t)w.pending];
    bool first_map = !w.mapped;
    bool resized = false;

    if (w.current >= 0) {
        const dm_buffer& ob = w.buffers[(size_t)w.current];
        resized = ob.width != nb.width || ob.height != nb.height;
        if (resized) {
            scene_damage_window(&w);
        }

        swp_release rel = { w.win_id, ob.buf_id };
        send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
    }

    if (first_map || resized) {
        damage_list::rect nr = { w.x - decor::BORDER,
                                 w.y - decor::TITLE_H,
                                 (int32_t)nb.width + 2 * decor::BORDER,
                                 (int32_t)nb.height + decor::TITLE_H
                                     + decor::BORDER };
        if (w.flags & SWP_WF_BORDERLESS) {
            nr = { w.x, w.y, (int32_t)nb.width, (int32_t)nb.height };
        }
        m_damage.add(nr.x, nr.y, nr.w, nr.h);
    } else if (w.pending_damage_count == 0) {
        m_damage.add(w.x, w.y, (int32_t)nb.width, (int32_t)nb.height);
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
    stlxgfx_fill_rect(back, r.x, r.y, (uint32_t)r.w, (uint32_t)r.h,
                      BACKGROUND);

    for (dm_window* w : m_zorder) {
        if (w->current < 0) {
            continue;
        }

        decor::draw(back, *w, w == m_focus, w == m_close_hover);

        dm_buffer& b = w->buffers[(size_t)w->current];
        int32_t ix0 = r.x > w->x ? r.x : w->x;
        int32_t iy0 = r.y > w->y ? r.y : w->y;
        int32_t ix1 = r.x + r.w < w->x + (int32_t)b.width
                    ? r.x + r.w : w->x + (int32_t)b.width;
        int32_t iy1 = r.y + r.h < w->y + (int32_t)b.height
                    ? r.y + r.h : w->y + (int32_t)b.height;
        if (ix0 >= ix1 || iy0 >= iy1) {
            continue;
        }

        stlxgfx_surface_t* src = stlxgfx_surface_from_buffer(
            reinterpret_cast<uint8_t*>(b.pixels), b.width, b.height,
            b.width * 4, 32, 16, 8, 0);
        if (src) {
            stlxgfx_blit(back, ix0, iy0, src,
                         ix0 - w->x, iy0 - w->y,
                         (uint32_t)(ix1 - ix0), (uint32_t)(iy1 - iy0));
            stlxgfx_destroy_surface(src);
        }
    }
}

void server::compose_tick() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->pending >= 0) {
                latch_window(*c, *w);
            }
        }
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
        damage_list::rect whole = { 0, 0, (int32_t)sw, (int32_t)sh };
        compose_rect(back, whole);
    } else {
        for (uint32_t i = 0; i < effective.count(); i++) {
            damage_list::rect r = effective.at(i);
            if (damage_list::clip(r, (int32_t)sw, (int32_t)sh)) {
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
}
