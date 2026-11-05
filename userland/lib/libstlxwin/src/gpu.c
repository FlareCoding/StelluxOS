#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <stlx/gpu_abi.h>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int restrict_handle(int gpu_fd, int handle, uint32_t rights) {
    struct gpu_restrict_args args;
    memset(&args, 0, sizeof(args));
    args.handle = handle;
    args.rights = rights;
    if (ioctl(gpu_fd, GPU_HANDLE_RESTRICT, &args) != 0) {
        return -1;
    }

    return args.restricted;
}

/* The rules of swp_attach_gpu_buffer's fields, which the DM disconnects for */
static int desc_fits_protocol(const stlxwin_gpu_buffer_desc* desc) {
    int width_fits = desc->width >= 1 && desc->width <= SWP_GPU_DIMENSION_MAX;
    int height_fits = desc->height >= 1 && desc->height <= SWP_GPU_DIMENSION_MAX;
    if (!width_fits || !height_fits) {
        return 0;
    }

    int rows_fit = (uint64_t)desc->pitch * desc->height <= UINT32_MAX;

    return desc->pitch % 4 == 0 && desc->pitch >= desc->width * 4 && rows_fit &&
           desc->format == SWP_FMT_XRGB8888 && desc->offset % 4 == 0;
}

static int open_gpu_device_locked(stlxwin_conn* conn) {
    if (conn->gpu_fd >= 0) {
        return 0;
    }

    char path[32];
    snprintf(path, sizeof(path), "/dev/gpu%u", conn->gpu_info.device);
    conn->gpu_fd = open(path, O_RDWR);

    return conn->gpu_fd >= 0 ? 0 : -1;
}

static stlxwin_gpu_record* find_record(stlxwin_conn* conn,
                                       const struct stlxwin_window* win,
                                       uint32_t buf_id) {
    for (uint32_t i = 0; i < SWP_GPU_BUFFERS_MAX; i++) {
        stlxwin_gpu_record* rec = &conn->gpu_records[i];
        if (rec->win == win && rec->buf_id == buf_id) {
            return rec;
        }
    }

    return NULL;
}

static stlxwin_gpu_record* find_free_record(stlxwin_conn* conn) {
    for (uint32_t i = 0; i < SWP_GPU_BUFFERS_MAX; i++) {
        if (!conn->gpu_records[i].win) {
            return &conn->gpu_records[i];
        }
    }

    return NULL;
}

static void free_record(stlxwin_gpu_record* rec) {
    close(rec->release);
    memset(rec, 0, sizeof(*rec));
}

/* The DM may show a detached buffer until its last release point */
static void drop_released_locked(stlxwin_conn* conn) {
    for (uint32_t i = 0; i < SWP_GPU_BUFFERS_MAX; i++) {
        stlxwin_gpu_record* rec = &conn->gpu_records[i];
        if (!rec->win || !rec->detached) {
            continue;
        }

        struct gpu_timeline_args query;
        memset(&query, 0, sizeof(query));
        query.timeline = rec->release;
        if (ioctl(conn->gpu_fd, GPU_TIMELINE_QUERY, &query) == 0 &&
            query.value >= rec->release_point) {
            free_record(rec);
        }
    }
}

/* An old-size picture after a resize must not ack the new size's configure */
static uint32_t ack_for_size(const struct stlxwin_window* win,
                             uint32_t width, uint32_t height) {
    for (uint32_t i = win->unacked_conf_count; i > 0; i--) {
        const stlxwin_unacked_conf* conf = &win->unacked_confs[i - 1];
        if (conf->width == width && conf->height == height) {
            return conf->serial;
        }
    }

    return win->acked_serial;
}

void stlxwin_gpu_release_records_locked(stlxwin_conn* conn,
                                        const struct stlxwin_window* win) {
    for (uint32_t i = 0; i < SWP_GPU_BUFFERS_MAX; i++) {
        stlxwin_gpu_record* rec = &conn->gpu_records[i];
        if (!rec->win || (win && rec->win != win)) {
            continue;
        }

        /* A failed signal is ignored, since only the client waits on the point */
        struct gpu_timeline_args signal;
        memset(&signal, 0, sizeof(signal));
        signal.timeline = rec->release;
        signal.value = rec->release_point;
        ioctl(conn->gpu_fd, GPU_TIMELINE_SIGNAL, &signal);
        free_record(rec);
    }
}

static uint32_t attach_locked(stlxwin_conn* conn, struct stlxwin_window* win,
                              const stlxwin_gpu_buffer_desc* desc) {
    int accepts = conn->gpu_info.flags & SWP_GPU_ACCEPTS;
    if (conn->dead || !accepts || open_gpu_device_locked(conn) != 0) {
        return 0;
    }

    drop_released_locked(conn);
    stlxwin_gpu_record* rec = find_free_record(conn);
    if (!rec) {
        return 0;
    }

    int own_release = restrict_handle(conn->gpu_fd, desc->release, GPU_RIGHT_READ | GPU_RIGHT_WRITE);
    int sent[SWP_GPU_HANDLES] = {
        restrict_handle(conn->gpu_fd, desc->buffer, GPU_RIGHT_READ),
        restrict_handle(conn->gpu_fd, desc->acquire, GPU_RIGHT_READ),
        restrict_handle(conn->gpu_fd, desc->release, GPU_RIGHT_WRITE),
    };

    swp_attach_gpu_buffer msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.buf_id = win->next_buf_id++;

    msg.width = desc->width;
    msg.height = desc->height;
    msg.pitch = desc->pitch;
    msg.format = desc->format;
    msg.offset = desc->offset;

    int is_restricted = own_release >= 0 && sent[0] >= 0 && sent[1] >= 0 && sent[2] >= 0;
    int is_sent = is_restricted &&
                  stlxwin_send_handles_locked(conn, SWP_MSG_ATTACH_GPU_BUFFER, &msg, sizeof(msg), sent) == 0;
    for (uint32_t i = 0; i < SWP_GPU_HANDLES; i++) {
        if (sent[i] >= 0) {
            close(sent[i]);
        }
    }

    if (!is_sent) {
        if (own_release >= 0) {
            close(own_release);
        }

        return 0;
    }

    rec->win = win;
    rec->buf_id = msg.buf_id;
    rec->width = desc->width;
    rec->height = desc->height;
    rec->release = own_release;

    return msg.buf_id;
}

uint32_t stlxwin_gpu_attach(stlxwin_window* win,
                            const stlxwin_gpu_buffer_desc* desc) {
    if (!win || !desc || !desc_fits_protocol(desc)) {
        return 0;
    }

    pthread_mutex_lock(&win->conn->lock);
    uint32_t buf_id = attach_locked(win->conn, win, desc);
    pthread_mutex_unlock(&win->conn->lock);

    return buf_id;
}

static int commit_locked(stlxwin_conn* conn, struct stlxwin_window* win,
                         swp_commit_gpu* msg, uint32_t flags) {
    if (conn->dead) {
        return -1;
    }

    drop_released_locked(conn);
    stlxwin_gpu_record* rec = find_record(conn, win, msg->buf_id);
    if (!rec || rec->detached) {
        return -1;
    }

    int points_rise = msg->release_point > rec->release_point &&
                      (msg->latch_point == 0 ||
                       (msg->latch_point > rec->release_point && msg->latch_point < msg->release_point));
    if (!points_rise) {
        return -1;
    }

    msg->ack_serial = ack_for_size(win, rec->width, rec->height);
    if (stlxwin_send_locked(conn, SWP_MSG_COMMIT_GPU, msg, sizeof(*msg)) != 0) {
        return -1;
    }

    rec->release_point = msg->release_point;
    stlxwin_window_record_ack_locked(win, msg->ack_serial);
    if (flags & STLXWIN_COMMIT_WANT_FRAME) {
        win->want_frame = 1;
    }

    return 0;
}

int stlxwin_gpu_commit(stlxwin_window* win, uint32_t buf_id,
                       uint64_t acquire_point, uint64_t latch_point,
                       uint64_t release_point, const stlxwin_rect* damage,
                       uint32_t n_damage, uint32_t flags) {
    if (!win) {
        return -1;
    }

    swp_commit_gpu msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.buf_id = buf_id;

    msg.acquire_point = acquire_point;
    msg.latch_point = latch_point;
    msg.release_point = release_point;
    msg.n_damage = stlxwin_wire_damage(msg.damage, damage, n_damage);

    pthread_mutex_lock(&win->conn->lock);
    int rc = commit_locked(win->conn, win, &msg, flags);
    pthread_mutex_unlock(&win->conn->lock);

    return rc;
}

static int detach_locked(stlxwin_conn* conn, struct stlxwin_window* win,
                         uint32_t buf_id) {
    stlxwin_gpu_record* rec = find_record(conn, win, buf_id);
    if (conn->dead || !rec || rec->detached) {
        return -1;
    }

    swp_detach_buffer msg = { win->win_id, buf_id };
    if (stlxwin_send_locked(conn, SWP_MSG_DETACH_BUFFER, &msg, sizeof(msg)) != 0) {
        return -1;
    }

    rec->detached = 1;
    drop_released_locked(conn);

    return 0;
}

int stlxwin_gpu_detach(stlxwin_window* win, uint32_t buf_id) {
    if (!win) {
        return -1;
    }

    pthread_mutex_lock(&win->conn->lock);
    int rc = detach_locked(win->conn, win, buf_id);
    pthread_mutex_unlock(&win->conn->lock);

    return rc;
}

/* A stream socket polls POLLHUP or POLLRDHUP once its peer is gone */
int stlxwin_gpu_check(stlxwin_conn* conn) {
    if (!conn) {
        return -1;
    }

    pthread_mutex_lock(&conn->lock);
    if (!conn->dead) {
        struct pollfd pfd = { conn->fd, POLLRDHUP, 0 };
        int is_gone = poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLRDHUP));
        if (is_gone) {
            stlxwin_mark_dead_locked(conn);
        } else {
            drop_released_locked(conn);
        }
    }

    int rc = conn->dead ? -1 : 0;
    pthread_mutex_unlock(&conn->lock);

    return rc;
}
