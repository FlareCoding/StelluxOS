#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fill_random_hex(char* dst, size_t chars) {
    uint8_t raw[8];
    if (chars > sizeof(raw) * 2) {
        return -1;
    }

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, raw, sizeof(raw));
    close(fd);
    if (n != (ssize_t)sizeof(raw)) {
        return -1;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < chars; i++) {
        dst[i] = hex[(raw[i / 2] >> ((i & 1) * 4)) & 0xF];
    }

    return 0;
}

/* Create, map, and attach one buffer sized to the window's current
 * configure. The fd closes right after mapping and the name belongs
 * to the display manager to unlink. */
static int slot_create(struct stlxwin_window* win, stlxwin_buf_slot* slot) {
    char name[SWP_SHM_NAME_MAX];
    char rand_hex[17] = {0};
    if (fill_random_hex(rand_hex, 16) != 0) {
        return -1;
    }
    snprintf(name, sizeof(name), "swb-%d-%s", getpid(), rand_hex);

    char path[SWP_SHM_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/dev/shm/%s", name);

    size_t size = (size_t)win->conf_w * win->conf_h * 4;
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0);
    if (fd < 0) {
        return -1;
    }

    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }

    void* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    close(fd);
    if (pixels == MAP_FAILED) {
        return -1;
    }

    slot->buf_id = win->next_buf_id++;
    slot->width = win->conf_w;
    slot->height = win->conf_h;
    slot->size = size;
    slot->pixels = pixels;
    slot->owned = 1;

    swp_attach_buffer msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.buf_id = slot->buf_id;
    msg.width = slot->width;
    msg.height = slot->height;
    strncpy(msg.shm_name, name, sizeof(msg.shm_name) - 1);
    if (stlxwin_send(win->conn, SWP_MSG_ATTACH_BUFFER,
                     &msg, sizeof(msg)) != 0) {
        munmap(pixels, size);
        slot->pixels = NULL;
        return -1;
    }

    return 0;
}

/* Owned slots may be unmapped immediately, the display manager holds
 * its own mapping until it processes the detach. */
static void slot_destroy(struct stlxwin_window* win,
                         stlxwin_buf_slot* slot) {
    if (!slot->pixels) {
        return;
    }

    swp_detach_buffer msg = { win->win_id, slot->buf_id };
    stlxwin_send(win->conn, SWP_MSG_DETACH_BUFFER, &msg, sizeof(msg));

    munmap(slot->pixels, slot->size);
    memset(slot, 0, sizeof(*slot));
}

void stlxwin_buffers_release_slot(struct stlxwin_window* win,
                                  uint32_t buf_id) {
    for (uint32_t i = 0; i < STLXWIN_BUFS_PER_WINDOW; i++) {
        if (win->bufs[i].pixels && win->bufs[i].buf_id == buf_id) {
            win->bufs[i].owned = 1;
            return;
        }
    }
}

void stlxwin_buffers_free_all(struct stlxwin_window* win) {
    for (uint32_t i = 0; i < STLXWIN_BUFS_PER_WINDOW; i++) {
        if (win->bufs[i].pixels) {
            munmap(win->bufs[i].pixels, win->bufs[i].size);
            memset(&win->bufs[i], 0, sizeof(win->bufs[i]));
        }
    }
}

/* An owned slot at the current size wins. An owned slot at a stale
 * size is recreated. An empty slot is filled on demand. */
static stlxwin_buf_slot* usable_slot(struct stlxwin_window* win) {
    stlxwin_buf_slot* empty = NULL;

    for (uint32_t i = 0; i < STLXWIN_BUFS_PER_WINDOW; i++) {
        stlxwin_buf_slot* s = &win->bufs[i];
        if (!s->pixels) {
            if (!empty) {
                empty = s;
            }
            continue;
        }

        if (!s->owned) {
            continue;
        }

        if (s->width == win->conf_w && s->height == win->conf_h) {
            return s;
        }

        slot_destroy(win, s);
        if (!empty) {
            empty = s;
        }
    }

    if (empty && slot_create(win, empty) == 0) {
        return empty;
    }

    return NULL;
}

static stlxwin_buffer* slot_view(stlxwin_buf_slot* slot) {
    slot->view.pixels = slot->pixels;
    slot->view.width = slot->width;
    slot->view.height = slot->height;
    slot->view.stride = slot->width * 4;
    return &slot->view;
}

stlxwin_buffer* stlxwin_try_begin_frame(stlxwin_window* win) {
    if (!win || win->dead || win->conn->dead) {
        return NULL;
    }

    stlxwin_dispatch(win->conn);

    stlxwin_buf_slot* slot = usable_slot(win);
    return slot ? slot_view(slot) : NULL;
}

stlxwin_buffer* stlxwin_begin_frame(stlxwin_window* win) {
    if (!win) {
        return NULL;
    }

    while (1) {
        stlxwin_buffer* buf = stlxwin_try_begin_frame(win);
        if (buf) {
            return buf;
        }

        if (win->dead || win->conn->dead) {
            return NULL;
        }

        struct pollfd pfd = { win->conn->fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, -1);
        if (rc < 0 && errno != EINTR) {
            win->conn->dead = 1;
            return NULL;
        }
    }
}

int stlxwin_commit(stlxwin_window* win, stlxwin_buffer* buf,
                   const stlxwin_rect* damage, uint32_t n_damage,
                   uint32_t flags) {
    if (!win || !buf || win->dead || win->conn->dead) {
        return -1;
    }

    stlxwin_buf_slot* slot = NULL;
    for (uint32_t i = 0; i < STLXWIN_BUFS_PER_WINDOW; i++) {
        if (&win->bufs[i].view == buf && win->bufs[i].owned) {
            slot = &win->bufs[i];
            break;
        }
    }
    if (!slot) {
        return -1;
    }

    swp_commit msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.buf_id = slot->buf_id;
    msg.ack_serial = win->conf_serial;

    /* More rects than the wire carries means the whole buffer */
    if (damage && n_damage > 0 && n_damage <= SWP_COMMIT_MAX_RECTS) {
        msg.n_damage = n_damage;
        for (uint32_t i = 0; i < n_damage; i++) {
            msg.damage[i].x = damage[i].x;
            msg.damage[i].y = damage[i].y;
            msg.damage[i].w = damage[i].w;
            msg.damage[i].h = damage[i].h;
        }
    }

    slot->owned = 0;
    if (flags & STLXWIN_COMMIT_WANT_FRAME) {
        win->want_frame = 1;
    }

    if (stlxwin_send(win->conn, SWP_MSG_COMMIT, &msg, sizeof(msg)) != 0) {
        return -1;
    }

    return 0;
}
