#include "screen.hpp"
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

        forget_window(c.windows[i].get());
        for (auto& b : c.windows[i]->buffers) {
            unmap_buffer(b);
        }
        c.windows.erase(c.windows.begin() + (long)i);
        m_scene_dirty = true;
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
        m_scene_dirty = true;
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
}

void server::present(const screen& scr, uint8_t* backbuffer) {
    /* Latch every pending commit and notify its owner */
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->pending < 0) {
                continue;
            }

            if (w->current >= 0) {
                swp_release rel = { w->win_id,
                                    w->buffers[(size_t)w->current].buf_id };
                send_to(*c, SWP_MSG_RELEASE, &rel, sizeof(rel));
            }

            w->current = w->pending;
            w->pending = -1;
            w->mapped = true;
            m_scene_dirty = true;

            swp_frame_done done = { w->win_id, 0, now_ns() };
            send_to(*c, SWP_MSG_FRAME_DONE, &done, sizeof(done));
        }
    }

    if (!m_scene_dirty) {
        return;
    }
    m_scene_dirty = false;

    /* Interim composition: background fill, window blits in creation
     * order, one full copy to scanout. The damage engine replaces this. */
    stlxgfx_surface_t* back = stlxgfx_surface_from_buffer(
        backbuffer, scr.width, scr.height, scr.width * 4, 32,
        scr.red_shift, scr.green_shift, scr.blue_shift);
    if (!back) {
        return;
    }

    stlxgfx_clear(back, BACKGROUND);

    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (!w->mapped || w->current < 0) {
                continue;
            }

            dm_buffer& b = w->buffers[(size_t)w->current];
            stlxgfx_surface_t* src = stlxgfx_surface_from_buffer(
                reinterpret_cast<uint8_t*>(b.pixels), b.width, b.height,
                b.width * 4, 32, 16, 8, 0);
            if (src) {
                stlxgfx_blit(back, w->x, w->y, src, 0, 0,
                             b.width, b.height);
                stlxgfx_destroy_surface(src);
            }
        }
    }

    for (uint32_t row = 0; row < scr.height; row++) {
        memcpy(scr.scanout + (size_t)row * scr.pitch,
               backbuffer + (size_t)row * scr.width * 4,
               (size_t)scr.width * 4);
    }

    stlxgfx_destroy_surface(back);
}
