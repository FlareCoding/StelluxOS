#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Blocking exact-count read, used only for the connect handshake */
static int read_full(int fd, void* dst, size_t len) {
    uint8_t* p = dst;
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }

            return -1;
        }

        got += (size_t)n;
    }

    return 0;
}

/* Blocking exact-count write. Client messages are small and the
 * display manager drains its socket every tick, so blocking here is
 * the simple and correct backpressure. */
static int write_full(int fd, const void* src, size_t len) {
    const uint8_t* p = src;
    size_t put = 0;

    while (put < len) {
        ssize_t n = write(fd, p + put, len - put);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }

            return -1;
        }

        put += (size_t)n;
    }

    return 0;
}

int stlxwin_send(stlxwin_conn* conn, uint16_t type,
                 const void* payload, uint32_t length) {
    if (conn->dead) {
        return -1;
    }

    swp_header hdr = { type, 0, length };
    if (write_full(conn->fd, &hdr, sizeof(hdr)) != 0 ||
        (length > 0 && write_full(conn->fd, payload, length) != 0)) {
        conn->dead = 1;
        return -1;
    }

    return 0;
}

int stlxwin_evq_push(stlxwin_conn* conn, const stlxwin_event* ev) {
    if (conn->evq_count == conn->evq_cap) {
        uint32_t new_cap = conn->evq_cap * 2;
        stlxwin_event* grown = malloc((size_t)new_cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }

        for (uint32_t i = 0; i < conn->evq_count; i++) {
            grown[i] = conn->evq[(conn->evq_head + i) % conn->evq_cap];
        }
        free(conn->evq);
        conn->evq = grown;
        conn->evq_cap = new_cap;
        conn->evq_head = 0;
    }

    uint32_t tail = (conn->evq_head + conn->evq_count) % conn->evq_cap;
    conn->evq[tail] = *ev;
    conn->evq_count++;
    return 0;
}

struct stlxwin_window* stlxwin_find_window(stlxwin_conn* conn,
                                           uint32_t win_id) {
    for (struct stlxwin_window* w = conn->windows; w; w = w->next) {
        if (w->win_id == win_id) {
            return w;
        }
    }

    return NULL;
}

/* Route one complete message into window state and the event queue */
static void handle_message(stlxwin_conn* conn, const swp_header* hdr,
                           const uint8_t* payload) {
    stlxwin_event ev;
    memset(&ev, 0, sizeof(ev));

    switch (hdr->type) {
    case SWP_MSG_CONFIGURE: {
        const swp_configure* m = (const swp_configure*)payload;
        struct stlxwin_window* w = stlxwin_find_window(conn, m->win_id);
        if (!w) {
            return;
        }

        w->conf_w = m->width;
        w->conf_h = m->height;
        w->conf_serial = m->serial;
        ev.type = STLXWIN_EVT_CONFIGURE;
        ev.window = w;
        ev.configure.width = m->width;
        ev.configure.height = m->height;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_RELEASE: {
        const swp_release* m = (const swp_release*)payload;
        struct stlxwin_window* w = stlxwin_find_window(conn, m->win_id);
        if (w) {
            stlxwin_buffers_release_slot(w, m->buf_id);
        }
        return;
    }
    case SWP_MSG_FRAME_DONE: {
        const swp_frame_done* m = (const swp_frame_done*)payload;
        struct stlxwin_window* w = stlxwin_find_window(conn, m->win_id);
        if (!w || !w->want_frame) {
            return;
        }

        w->want_frame = 0;
        ev.type = STLXWIN_EVT_FRAME;
        ev.window = w;
        ev.frame.time_ns = m->time_ns;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_EVENT: {
        const swp_event_prefix* pre = (const swp_event_prefix*)payload;
        struct stlxwin_window* w = stlxwin_find_window(conn, pre->win_id);
        if (!w) {
            return;
        }

        const swp_event_rec* rec =
            (const swp_event_rec*)(payload + sizeof(*pre));
        for (uint32_t i = 0; i < pre->count; i++, rec++) {
            memset(&ev, 0, sizeof(ev));
            ev.window = w;
            switch (rec->kind) {
            case SWP_EV_KEY_DOWN:   ev.type = STLXWIN_EVT_KEY_DOWN; break;
            case SWP_EV_KEY_UP:     ev.type = STLXWIN_EVT_KEY_UP; break;
            case SWP_EV_KEY_REPEAT: ev.type = STLXWIN_EVT_KEY_REPEAT; break;
            case SWP_EV_MOTION:     ev.type = STLXWIN_EVT_POINTER_MOTION; break;
            case SWP_EV_BUTTON_DOWN:ev.type = STLXWIN_EVT_BUTTON_DOWN; break;
            case SWP_EV_BUTTON_UP:  ev.type = STLXWIN_EVT_BUTTON_UP; break;
            case SWP_EV_SCROLL:     ev.type = STLXWIN_EVT_SCROLL; break;
            case SWP_EV_ENTER:      ev.type = STLXWIN_EVT_POINTER_ENTER; break;
            case SWP_EV_LEAVE:      ev.type = STLXWIN_EVT_POINTER_LEAVE; break;
            case SWP_EV_FOCUS_IN:   ev.type = STLXWIN_EVT_FOCUS_IN; break;
            case SWP_EV_FOCUS_OUT:  ev.type = STLXWIN_EVT_FOCUS_OUT; break;
            case SWP_EV_CLOSE:      ev.type = STLXWIN_EVT_CLOSE; break;
            case SWP_EV_POPUP_DISMISSED:
                ev.type = STLXWIN_EVT_POPUP_DISMISSED;
                break;
            default:
                continue;
            }

            switch (ev.type) {
            case STLXWIN_EVT_KEY_DOWN:
            case STLXWIN_EVT_KEY_UP:
            case STLXWIN_EVT_KEY_REPEAT:
                ev.key.usage = rec->usage;
                ev.key.modifiers = rec->modifiers;
                ev.key.ch = rec->ch;
                break;
            case STLXWIN_EVT_POINTER_MOTION:
            case STLXWIN_EVT_POINTER_ENTER:
            case STLXWIN_EVT_POINTER_LEAVE:
                ev.motion.x = rec->x;
                ev.motion.y = rec->y;
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
            case STLXWIN_EVT_BUTTON_UP:
                ev.button.x = rec->x;
                ev.button.y = rec->y;
                ev.button.button = rec->button;
                break;
            case STLXWIN_EVT_SCROLL:
                ev.scroll.x = rec->x;
                ev.scroll.y = rec->y;
                ev.scroll.dy = rec->scroll;
                break;
            default:
                break;
            }

            stlxwin_evq_push(conn, &ev);
        }
        return;
    }
    case SWP_MSG_CLIPBOARD_DATA: {
        const swp_clipboard_data* m = (const swp_clipboard_data*)payload;
        free(conn->clip_text);
        conn->clip_text = NULL;
        conn->clip_len = 0;
        if (m->len > 0 && m->len <= SWP_CLIPBOARD_MAX) {
            conn->clip_text = malloc(m->len);
            if (conn->clip_text) {
                memcpy(conn->clip_text, payload + sizeof(*m), m->len);
                conn->clip_len = m->len;
            }
        }
        conn->clip_pending = 0;
        return;
    }
    default:
        return;
    }
}

/* Pull everything available off the socket, assembling messages across
 * short reads. Returns events queued, or -1 when the connection died. */
int stlxwin_dispatch(stlxwin_conn* conn) {
    if (!conn || conn->dead) {
        return -1;
    }

    uint32_t before = conn->evq_count;

    while (1) {
        ssize_t n = read(conn->fd, conn->rd_buf + conn->rd_have,
                         sizeof(conn->rd_buf) - conn->rd_have);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            conn->dead = 1;
            break;
        }

        if (n == 0) {
            conn->dead = 1;
            break;
        }

        conn->rd_have += (uint32_t)n;

        /* Consume every complete message in the buffer */
        uint32_t off = 0;
        while (conn->rd_have - off >= sizeof(swp_header)) {
            swp_header hdr;
            memcpy(&hdr, conn->rd_buf + off, sizeof(hdr));
            if (hdr.length > SWP_MAX_MSG_SIZE - sizeof(hdr)) {
                conn->dead = 1;
                break;
            }

            if (conn->rd_have - off < sizeof(hdr) + hdr.length) {
                break;
            }

            handle_message(conn, &hdr, conn->rd_buf + off + sizeof(hdr));
            off += sizeof(hdr) + hdr.length;
        }

        if (off > 0) {
            memmove(conn->rd_buf, conn->rd_buf + off, conn->rd_have - off);
            conn->rd_have -= off;
        }
    }

    if (conn->dead) {
        stlxwin_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = STLXWIN_EVT_DISCONNECTED;
        stlxwin_evq_push(conn, &ev);
    }

    return (int)(conn->evq_count - before);
}

int stlxwin_wait(stlxwin_conn* conn, int64_t timeout_ns) {
    if (!conn) {
        return -1;
    }

    /* Messages that update state without queueing an event, releases
     * for example, wake the poll but must not end an infinite wait */
    while (1) {
        if (conn->evq_count > 0) {
            return 1;
        }
        if (conn->dead) {
            return -1;
        }

        struct pollfd pfd = { conn->fd, POLLIN, 0 };
        int timeout_ms = -1;
        if (timeout_ns >= 0) {
            timeout_ms = (int)(timeout_ns / 1000000);
        }

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno != EINTR) {
            conn->dead = 1;
            return -1;
        }

        if (rc == 0) {
            return 0;
        }

        if (rc > 0) {
            stlxwin_dispatch(conn);
        }

        if (timeout_ns >= 0) {
            return conn->evq_count > 0 ? 1 : (conn->dead ? -1 : 0);
        }
    }
}

int stlxwin_next_event(stlxwin_conn* conn, stlxwin_event* out) {
    if (!conn || conn->evq_count == 0) {
        return 0;
    }

    *out = conn->evq[conn->evq_head];
    conn->evq_head = (conn->evq_head + 1) % conn->evq_cap;
    conn->evq_count--;
    return 1;
}

int stlxwin_conn_fd(const stlxwin_conn* conn) {
    return conn ? conn->fd : -1;
}

stlxwin_conn* stlxwin_connect(const char* app_id) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SWP_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    /* Synchronous hello handshake, then the socket goes nonblocking
     * for the lifetime of the connection */
    swp_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.proto_version = SWP_VERSION;
    if (app_id) {
        strncpy(hello.app_id, app_id, sizeof(hello.app_id) - 1);
    }

    swp_header hdr = { SWP_MSG_HELLO, 0, sizeof(hello) };
    if (write_full(fd, &hdr, sizeof(hdr)) != 0 ||
        write_full(fd, &hello, sizeof(hello)) != 0) {
        close(fd);
        return NULL;
    }

    swp_header rhdr;
    swp_hello_reply reply;
    if (read_full(fd, &rhdr, sizeof(rhdr)) != 0 ||
        rhdr.type != SWP_MSG_HELLO_REPLY ||
        rhdr.length != sizeof(reply) ||
        read_full(fd, &reply, sizeof(reply)) != 0 ||
        reply.proto_version != SWP_VERSION) {
        close(fd);
        return NULL;
    }

    stlxwin_conn* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        close(fd);
        return NULL;
    }

    conn->evq = malloc(STLXWIN_EVQ_INITIAL * sizeof(*conn->evq));
    if (!conn->evq) {
        free(conn);
        close(fd);
        return NULL;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);

    conn->fd = fd;
    conn->evq_cap = STLXWIN_EVQ_INITIAL;
    conn->next_win_id = 1;
    conn->screen_w = reply.screen_w;
    conn->screen_h = reply.screen_h;

    return conn;
}

void stlxwin_disconnect(stlxwin_conn* conn) {
    if (!conn) {
        return;
    }

    while (conn->windows) {
        struct stlxwin_window* w = conn->windows;
        conn->windows = w->next;
        stlxwin_buffers_free_all(w);
        stlxwin_window_free(w);
    }

    close(conn->fd);
    free(conn->clip_text);
    free(conn->evq);
    free(conn);
}
