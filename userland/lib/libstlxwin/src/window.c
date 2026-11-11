#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <stdlib.h>
#include <string.h>

static uint32_t count_windows(const stlxwin_conn* conn) {
    uint32_t count = 0;
    for (const stlxwin_window* win = conn->windows; win; win = win->next) {
        count++;
    }

    return count;
}

static stlxwin_window* window_new(stlxwin_conn* conn, uint32_t parent_id,
                                  int32_t rel_x, int32_t rel_y,
                                  uint32_t width, uint32_t height,
                                  const char* title,
                                  uint32_t flags, uint32_t popup_flags) {
    if (!conn || conn->dead || width == 0 || height == 0) {
        return NULL;
    }

    /* Each window holds two shared memory buffers, and the display manager
     * disconnects a connection whose attach would pass SWP_SHM_BUFFERS_MAX */
    if (count_windows(conn) == STLXWIN_WINDOWS_MAX) {
        return NULL;
    }

    stlxwin_window* win = calloc(1, sizeof(*win));
    if (!win) {
        return NULL;
    }

    win->conn = conn;
    win->win_id = conn->next_win_id++;
    win->parent_id = parent_id;
    win->conf_w = width;
    win->conf_h = height;
    win->next_buf_id = 1;

    swp_create_window msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.width = width;
    msg.height = height;
    msg.flags = flags;
    msg.parent_id = parent_id;
    msg.rel_x = rel_x;
    msg.rel_y = rel_y;
    msg.popup_flags = popup_flags;
    if (title) {
        strncpy(msg.title, title, sizeof(msg.title) - 1);
    }

    if (stlxwin_send(conn, SWP_MSG_CREATE_WINDOW, &msg, sizeof(msg)) != 0) {
        free(win);
        return NULL;
    }

    win->next = conn->windows;
    conn->windows = win;
    return win;
}

void stlxwin_window_keep_configure_locked(struct stlxwin_window* win,
                                          const swp_configure* conf) {
    win->conf_w = conf->width;
    win->conf_h = conf->height;
    win->conf_serial = conf->serial;

    if (win->unacked_conf_count == STLXWIN_UNACKED_CONFS_MAX) {
        memmove(&win->unacked_confs[0], &win->unacked_confs[1],
                (STLXWIN_UNACKED_CONFS_MAX - 1) * sizeof(win->unacked_confs[0]));
        win->unacked_conf_count--;
    }

    stlxwin_unacked_conf* kept = &win->unacked_confs[win->unacked_conf_count++];
    kept->serial = conf->serial;
    kept->width = conf->width;
    kept->height = conf->height;
}

void stlxwin_window_record_ack_locked(struct stlxwin_window* win,
                                      uint32_t serial) {
    if (serial > win->acked_serial) {
        win->acked_serial = serial;
    }

    uint32_t kept = 0;
    for (uint32_t i = 0; i < win->unacked_conf_count; i++) {
        if (win->unacked_confs[i].serial > win->acked_serial) {
            win->unacked_confs[kept++] = win->unacked_confs[i];
        }
    }

    win->unacked_conf_count = kept;
}

stlxwin_window* stlxwin_window_create(stlxwin_conn* conn,
                                      uint32_t width, uint32_t height,
                                      const char* title, uint32_t flags) {
    return window_new(conn, 0, 0, 0, width, height, title, flags, 0);
}

stlxwin_window* stlxwin_popup_create(stlxwin_window* parent,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t flags) {
    if (!parent || parent->dead) {
        return NULL;
    }

    uint32_t window_flags = SWP_WF_BORDERLESS;
    if (flags & STLXWIN_PF_GLASS) {
        window_flags |= SWP_WF_GLASS;
    }

    if (flags & STLXWIN_PF_PREMULTIPLIED) {
        window_flags |= SWP_WF_PREMULTIPLIED;
    }

    uint32_t popup_flags = flags & STLXWIN_PF_GRAB;
    if (flags & STLXWIN_PF_ROUNDED) {
        popup_flags |= SWP_PF_ROUNDED;
    }

    return window_new(parent->conn, parent->win_id, x, y, width, height, NULL, window_flags, popup_flags);
}

void stlxwin_window_free(stlxwin_window* win) {
    free(win);
}

/* Queued events hold window pointers, so a dying window's events are
 * dropped in place before the memory goes away. */
static void evq_scrub(stlxwin_conn* conn, const stlxwin_window* win) {
    uint32_t kept = 0;

    for (uint32_t i = 0; i < conn->evq_count; i++) {
        stlxwin_event* ev =
            &conn->evq[(conn->evq_head + i) % conn->evq_cap];
        if (ev->window != win) {
            conn->evq[(conn->evq_head + kept) % conn->evq_cap] = *ev;
            kept++;
        }
    }

    conn->evq_count = kept;
}

void stlxwin_window_destroy(stlxwin_window* win) {
    if (!win) {
        return;
    }

    stlxwin_conn* conn = win->conn;

    /* Children first, the display manager destroys them with the
     * parent and the client must match */
    for (stlxwin_window* w = conn->windows; w;) {
        stlxwin_window* next = w->next;
        if (w->parent_id == win->win_id) {
            stlxwin_window_destroy(w);
        }
        w = next;
    }

    /* The release points are reached only once the destroy is sent, so a
     * picture drawn too early tears only a window that is going away */
    swp_destroy_window msg = { win->win_id };
    pthread_mutex_lock(&conn->lock);
    stlxwin_send_locked(conn, SWP_MSG_DESTROY_WINDOW, &msg, sizeof(msg));
    stlxwin_gpu_release_records_locked(conn, win);
    pthread_mutex_unlock(&conn->lock);

    stlxwin_window** link = &conn->windows;
    while (*link && *link != win) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = win->next;
    }

    evq_scrub(conn, win);
    stlxwin_buffers_free_all(win);
    free(win);
}

static void set_field(stlxwin_window* win, uint32_t field,
                      uint32_t a, uint32_t b, const char* title) {
    if (!win || win->dead) {
        return;
    }

    swp_set_window msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.field = field;
    msg.a = a;
    msg.b = b;
    if (title) {
        strncpy(msg.title, title, sizeof(msg.title) - 1);
    }

    stlxwin_send(win->conn, SWP_MSG_SET_WINDOW, &msg, sizeof(msg));
}

void stlxwin_window_set_title(stlxwin_window* win, const char* title) {
    set_field(win, SWP_FIELD_TITLE, 0, 0, title ? title : "");
}

void stlxwin_window_set_min_size(stlxwin_window* win,
                                 uint32_t w, uint32_t h) {
    set_field(win, SWP_FIELD_MIN_SIZE, w, h, NULL);
}

void stlxwin_window_set_max_size(stlxwin_window* win,
                                 uint32_t w, uint32_t h) {
    set_field(win, SWP_FIELD_MAX_SIZE, w, h, NULL);
}

void stlxwin_window_set_fullscreen(stlxwin_window* win, int enabled) {
    set_field(win, SWP_FIELD_FULLSCREEN, enabled ? 1u : 0u, 0, NULL);
}

void stlxwin_window_set_cursor(stlxwin_window* win, stlxwin_cursor cursor) {
    set_field(win, SWP_FIELD_CURSOR, (uint32_t)cursor, 0, NULL);
}

void stlxwin_window_activate(stlxwin_window* win) {
    if (!win || win->dead) {
        return;
    }

    swp_activate_window msg = { win->win_id };
    stlxwin_send(win->conn, SWP_MSG_ACTIVATE_WINDOW, &msg, sizeof(msg));
}

void stlxwin_window_get_size(const stlxwin_window* win,
                             uint32_t* w, uint32_t* h) {
    if (w) {
        *w = win ? win->conf_w : 0;
    }
    if (h) {
        *h = win ? win->conf_h : 0;
    }
}

int stlxwin_window_get_output(const stlxwin_window* win, uint32_t* output,
                              int32_t* x, int32_t* y) {
    if (!win || !win->has_output) {
        return -1;
    }

    if (output) {
        *output = win->output;
    }

    if (x) {
        *x = win->desktop_x;
    }

    if (y) {
        *y = win->desktop_y;
    }

    return 0;
}

void stlxwin_window_configured(stlxwin_window* win, uint32_t* width,
                               uint32_t* height, uint32_t* serial) {
    uint32_t conf_w = 0;
    uint32_t conf_h = 0;
    uint32_t conf_serial = 0;
    if (win) {
        pthread_mutex_lock(&win->conn->lock);
        conf_w = win->conf_w;
        conf_h = win->conf_h;
        conf_serial = win->conf_serial;
        pthread_mutex_unlock(&win->conn->lock);
    }

    if (width) {
        *width = conf_w;
    }

    if (height) {
        *height = conf_h;
    }

    if (serial) {
        *serial = conf_serial;
    }
}
