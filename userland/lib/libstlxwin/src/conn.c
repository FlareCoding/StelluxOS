#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The longest a message waits for room in a full socket before the connection counts as dead */
#define SEND_WAIT_NS 100000000ll

/* Room for the most handles one message carries. The kernel fits `(controllen - sizeof(cmsghdr)) / 4` handles, so
 * CMSG_SPACE's padding would fit one more. */
#define HANDLE_CONTROL_BYTES CMSG_LEN(SWP_HANDLES_MAX * sizeof(int))

_Static_assert(STLXWIN_MODES_MAX == SWP_MODES_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_OUTPUTS_MAX == SWP_OUTPUTS_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_OUTPUT_LIT == SWP_OUTPUT_LIT, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_OUTPUT_PRIMARY == SWP_OUTPUT_PRIMARY, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_SET_MODE_TRIAL == SWP_SET_MODE_TRIAL, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_MODE_SET == SWP_MODE_SET, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_TRIAL == SWP_MODE_TRIAL, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_KEPT == SWP_MODE_KEPT, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_SET_BACK == SWP_MODE_SET_BACK, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_DEFAULT == SWP_MODE_DEFAULT, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_NO_BUFFERS == SWP_MODE_NO_BUFFERS, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_BUSY == SWP_MODE_BUSY, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_NO_TRIAL == SWP_MODE_NO_TRIAL, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_MODE_INVALID == SWP_MODE_INVALID, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_GPU_ACCEPTS == SWP_GPU_ACCEPTS, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_GPU_CPU_READS == SWP_GPU_CPU_READS, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_GPU_GPU_READS == SWP_GPU_GPU_READS, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_WINDOWS_MAX * STLXWIN_BUFS_PER_WINDOW <= SWP_SHM_BUFFERS_MAX,
               "a connection's windows hold no more shared memory buffers than proto.h's limit");
_Static_assert(STLXWIN_GPU_BUFFERS_MAX == SWP_GPU_BUFFERS_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_GPU_DIMENSION_MAX == SWP_GPU_DIMENSION_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_FMT_XRGB8888 == SWP_FMT_XRGB8888, "stlxwin.h names proto.h's format");
_Static_assert(STLXWIN_WF_RESIZABLE == SWP_WF_RESIZABLE, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_WF_BORDERLESS == SWP_WF_BORDERLESS, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_WF_GLASS == SWP_WF_GLASS, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_WF_PREMULTIPLIED == SWP_WF_PREMULTIPLIED, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_PF_GRAB == SWP_PF_GRAB, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_DESKTOP_MAIN == SWP_DESKTOP_MAIN, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_LAYOUT_APPLIED == SWP_LAYOUT_APPLIED, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_LAYOUT_UNSAVED == SWP_LAYOUT_UNSAVED, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_LAYOUT_STALE == SWP_LAYOUT_STALE, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_LAYOUT_INVALID == SWP_LAYOUT_INVALID, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_LAYOUT_OVERLAP == SWP_LAYOUT_OVERLAP, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_LAYOUT_APART == SWP_LAYOUT_APART, "stlxwin.h names proto.h's outcome");
_Static_assert(STLXWIN_OFFER_TYPES_MAX == SWP_OFFER_TYPES_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_MIME_MAX == SWP_MIME_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_CLIPBOARD_PRIVATE == SWP_CLIPBOARD_PRIVATE, "stlxwin.h names proto.h's flag");
_Static_assert(sizeof(stlxwin_offer_types) == sizeof(swp_clipboard_types), "the types copy whole");
_Static_assert(STLXWIN_DRAG_PICTURE_MAX == SWP_DRAG_PICTURE_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_DROP_WORDS_MAX == SWP_DROP_WORDS_MAX, "stlxwin.h names proto.h's limit");
_Static_assert(STLXWIN_DND_COPY == SWP_DND_COPY, "stlxwin.h names proto.h's action");
_Static_assert(STLXWIN_DND_MOVE == SWP_DND_MOVE, "stlxwin.h names proto.h's action");
_Static_assert(STLXWIN_DND_LINK == SWP_DND_LINK, "stlxwin.h names proto.h's action");
_Static_assert(STLXWIN_DRAG_STARTED == SWP_DRAG_STARTED, "stlxwin.h names proto.h's state");
_Static_assert(STLXWIN_DRAG_REFUSED == SWP_DRAG_REFUSED, "stlxwin.h names proto.h's state");
_Static_assert(STLXWIN_DRAG_FINISHED == SWP_DRAG_FINISHED, "stlxwin.h names proto.h's state");
_Static_assert(STLXWIN_DRAG_CANCELLED == SWP_DRAG_CANCELLED, "stlxwin.h names proto.h's state");
_Static_assert(STLXWIN_DROP_OWN == SWP_DROP_OWN, "stlxwin.h names proto.h's flag");
_Static_assert(STLXWIN_DROP_FOCUS == SWP_DROP_FOCUS, "stlxwin.h names proto.h's flag");

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

static int64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static int wait_writable(int fd, int64_t* deadline_ns) {
    int64_t now = monotonic_ns();
    if (*deadline_ns == 0) {
        *deadline_ns = now + SEND_WAIT_NS;
    }

    while (now < *deadline_ns) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int rc = poll(&pfd, 1, (int)((*deadline_ns - now) / 1000000) + 1);
        if (rc > 0) {
            return (pfd.revents & POLLOUT) ? 0 : -1;
        }

        if (rc < 0 && errno != EINTR) {
            return -1;
        }

        now = monotonic_ns();
    }

    return -1;
}

/* Exact-count write that waits for room in a full socket only until the message's deadline */
static int write_all(int fd, const void* src, size_t len, int64_t* deadline_ns) {
    const uint8_t* p = src;
    size_t put = 0;

    while (put < len) {
        ssize_t n = send(fd, p + put, len - put, MSG_NOSIGNAL);
        if (n > 0) {
            put += (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        int is_full = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        if (!is_full || wait_writable(fd, deadline_ns) != 0) {
            return -1;
        }
    }

    return 0;
}

int stlxwin_send_locked(stlxwin_conn* conn, uint16_t type,
                        const void* payload, uint32_t length) {
    if (conn->dead) {
        return -1;
    }

    int64_t deadline_ns = 0;
    swp_header hdr = { type, 0, length };
    if (write_all(conn->fd, &hdr, sizeof(hdr), &deadline_ns) != 0 ||
        (length > 0 && write_all(conn->fd, payload, length, &deadline_ns) != 0)) {
        stlxwin_mark_dead_locked(conn);
        return -1;
    }

    return 0;
}

int stlxwin_send(stlxwin_conn* conn, uint16_t type,
                 const void* payload, uint32_t length) {
    pthread_mutex_lock(&conn->lock);
    int rc = stlxwin_send_locked(conn, type, payload, length);
    pthread_mutex_unlock(&conn->lock);

    return rc;
}

/* The handles go with the first byte that goes out, and a sendmsg refused whole is retried whole, since the kernel
 * drops its batch. No message carries more handles than a START_DRAG. */
int stlxwin_send_handles_locked(stlxwin_conn* conn, uint16_t type,
                                const void* payload, uint32_t length,
                                const int* handles, uint32_t handle_count) {
    if (conn->dead || length > SWP_MAX_MSG_SIZE - sizeof(swp_header) ||
        handle_count == 0 || handle_count > SWP_HANDLES_MAX) {
        return -1;
    }

    uint8_t bytes[SWP_MAX_MSG_SIZE];
    swp_header hdr = { type, 0, length };
    memcpy(bytes, &hdr, sizeof(hdr));
    memcpy(bytes + sizeof(hdr), payload, length);

    union {
        struct cmsghdr head;
        uint8_t        space[CMSG_SPACE(SWP_HANDLES_MAX * sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));

    struct iovec iov = { bytes, sizeof(hdr) + length };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = CMSG_SPACE(handle_count * sizeof(int));

    struct cmsghdr* head = CMSG_FIRSTHDR(&msg);
    head->cmsg_level = SOL_SOCKET;
    head->cmsg_type = SCM_RIGHTS;
    head->cmsg_len = CMSG_LEN(handle_count * sizeof(int));
    memcpy(CMSG_DATA(head), handles, handle_count * sizeof(int));

    int64_t deadline_ns = 0;
    while (1) {
        ssize_t n = sendmsg(conn->fd, &msg, MSG_NOSIGNAL);
        if (n > 0 && write_all(conn->fd, bytes + n, iov.iov_len - (size_t)n, &deadline_ns) == 0) {
            return 0;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        int is_full = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        if (!is_full || wait_writable(conn->fd, &deadline_ns) != 0) {
            break;
        }
    }

    stlxwin_mark_dead_locked(conn);

    return -1;
}

/* A renderer may wait on a release point the DM can no longer reach,
 * so the library reaches every one it holds as the connection dies */
void stlxwin_mark_dead_locked(stlxwin_conn* conn) {
    if (conn->dead) {
        return;
    }

    conn->dead = 1;
    stlxwin_gpu_release_records_locked(conn, NULL);
}

void stlxwin_mark_dead(stlxwin_conn* conn) {
    pthread_mutex_lock(&conn->lock);
    stlxwin_mark_dead_locked(conn);
    pthread_mutex_unlock(&conn->lock);
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

/* Reads the GPU_INFO a HELLO_REPLY's flag announces. A longer one is
 * taken, since proto.h lets fields be appended past swp_gpu_info's. */
static int read_hello_gpu_info(int fd, swp_gpu_info* info) {
    swp_header hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) != 0 ||
        hdr.type != SWP_MSG_GPU_INFO ||
        hdr.length < sizeof(*info) ||
        hdr.length > SWP_MAX_MSG_SIZE - sizeof(hdr)) {
        return -1;
    }

    uint8_t payload[SWP_MAX_MSG_SIZE];
    if (read_full(fd, payload, hdr.length) != 0) {
        return -1;
    }

    memcpy(info, payload, sizeof(*info));

    return 0;
}

/* Keeps an OUTPUTS whose count fits its list. Returns 0, or -1 when malformed */
static int keep_outputs(swp_outputs* kept, const swp_header* hdr, const uint8_t* payload) {
    swp_outputs m;
    if (hdr->type != SWP_MSG_OUTPUTS || hdr->length != sizeof(m)) {
        return -1;
    }

    memcpy(&m, payload, sizeof(m));
    if (m.count > SWP_OUTPUTS_MAX) {
        return -1;
    }

    *kept = m;

    return 0;
}

static int read_hello_outputs(int fd, swp_outputs* outputs) {
    swp_header hdr;
    uint8_t payload[sizeof(*outputs)];
    if (read_full(fd, &hdr, sizeof(hdr)) != 0 || hdr.length != sizeof(payload) ||
        read_full(fd, payload, sizeof(payload)) != 0) {
        return -1;
    }

    return keep_outputs(outputs, &hdr, payload);
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

/* Queues the handles of a read's one SCM_RIGHTS message, closing any past the queue's room */
static void queue_received_handles(stlxwin_conn* conn, const struct msghdr* msg) {
    const struct cmsghdr* head = CMSG_FIRSTHDR(msg);
    if (!head || head->cmsg_level != SOL_SOCKET || head->cmsg_type != SCM_RIGHTS ||
        head->cmsg_len < CMSG_LEN(0)) {
        return;
    }

    size_t count = (head->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; i++) {
        int handle;
        memcpy(&handle, CMSG_DATA(head) + i * sizeof(int), sizeof(handle));
        if (conn->queued_handle_count == STLXWIN_QUEUED_HANDLES_MAX) {
            close(handle);
            continue;
        }

        conn->queued_handles[conn->queued_handle_count++] = handle;
    }
}

static int take_queued_handle(stlxwin_conn* conn) {
    if (conn->queued_handle_count == 0) {
        return -1;
    }

    int handle = conn->queued_handles[0];
    conn->queued_handle_count--;
    memmove(conn->queued_handles, conn->queued_handles + 1, conn->queued_handle_count * sizeof(int));

    return handle;
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

        pthread_mutex_lock(&conn->lock);
        stlxwin_window_keep_configure_locked(w, m);
        pthread_mutex_unlock(&conn->lock);

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
        if (!w) {
            return;
        }

        pthread_mutex_lock(&conn->lock);
        int wanted = w->want_frame;
        w->want_frame = 0;
        pthread_mutex_unlock(&conn->lock);

        if (!wanted) {
            return;
        }

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
            case SWP_EV_PASTE:      ev.type = STLXWIN_EVT_PASTE; break;
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
                ev.motion.modifiers = rec->modifiers;
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
            case STLXWIN_EVT_BUTTON_UP:
                ev.button.x = rec->x;
                ev.button.y = rec->y;
                ev.button.button = rec->button;
                ev.button.modifiers = rec->modifiers;
                ev.button.serial = ev.type == STLXWIN_EVT_BUTTON_DOWN ? rec->ch : 0;
                break;
            case STLXWIN_EVT_SCROLL:
                ev.scroll.x = rec->x;
                ev.scroll.y = rec->y;
                ev.scroll.dy = rec->scroll;
                ev.scroll.modifiers = rec->modifiers;
                break;
            default:
                break;
            }

            stlxwin_evq_push(conn, &ev);
        }
        return;
    }
    case SWP_MSG_CLIPBOARD_DATA: {
        swp_clipboard_data m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        if (conn->clip_handle >= 0) {
            close(conn->clip_handle);
        }

        conn->clip_len = m.len;
        conn->clip_handle = m.len > 0 ? take_queued_handle(conn) : -1;
        conn->clip_pending = 0;
        return;
    }
    case SWP_MSG_DROP_DATA: {
        swp_drop_data m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        if (conn->drop_handle >= 0) {
            close(conn->drop_handle);
        }

        conn->drop_len = m.len;
        conn->drop_handle = m.len > 0 ? take_queued_handle(conn) : -1;
        conn->drop_pending = 0;
        return;
    }
    case SWP_MSG_DRAG_STATE:
    case SWP_MSG_DROP_ENTER:
    case SWP_MSG_DROP_MOTION:
    case SWP_MSG_DROP_LEAVE:
    case SWP_MSG_DROP:
        stlxwin_dnd_take_message(conn, hdr, payload);
        return;
    case SWP_MSG_CLIPBOARD_TYPES: {
        swp_clipboard_types m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        if (m.count > SWP_OFFER_TYPES_MAX) {
            return;
        }

        for (uint32_t i = 0; i < SWP_OFFER_TYPES_MAX; i++) {
            m.types[i][SWP_MIME_MAX - 1] = '\0';
        }

        /* The first answers the watch, which waits for it, and each later one is a change */
        conn->clip_types = m;
        conn->clip_types_flags = hdr->flags;
        if (!conn->has_clip_types) {
            conn->has_clip_types = 1;
            return;
        }

        ev.type = STLXWIN_EVT_CLIPBOARD;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_MODES: {
        /* A payload may sit at any offset of the read buffer, so it is copied rather than cast */
        swp_modes m;
        if (hdr->length != sizeof(m) || !conn->modes) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        if (m.output >= SWP_OUTPUTS_MAX || m.count > SWP_MODES_MAX) {
            return;
        }

        conn->modes[m.output] = m;
        conn->modes_kept |= 1u << m.output;
        ev.type = STLXWIN_EVT_MODES;
        ev.modes.output = m.output;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_MODE_RESULT: {
        swp_mode_result m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        ev.type = STLXWIN_EVT_MODE_RESULT;
        ev.mode_result.output = m.output;
        ev.mode_result.result = m.result;
        ev.mode_result.outcome = m.outcome;
        ev.mode_result.mode = stlxwin_mode_from_swp(&m.mode);

        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_GPU_INFO: {
        if (hdr->length < sizeof(conn->gpu_info)) {
            return;
        }

        pthread_mutex_lock(&conn->lock);
        memcpy(&conn->gpu_info, payload, sizeof(conn->gpu_info));
        pthread_mutex_unlock(&conn->lock);

        ev.type = STLXWIN_EVT_GPU_INFO;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_OUTPUTS:
        if (keep_outputs(&conn->outputs, hdr, payload) != 0) {
            return;
        }

        ev.type = STLXWIN_EVT_OUTPUTS;
        stlxwin_evq_push(conn, &ev);
        return;
    case SWP_MSG_WINDOW_OUTPUT: {
        swp_window_output m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        struct stlxwin_window* w = stlxwin_find_window(conn, m.win_id);
        if (!w || w->parent_id != 0) {
            return;
        }

        w->has_output = 1;
        w->output = m.output;
        w->desktop_x = m.x;
        w->desktop_y = m.y;

        ev.type = STLXWIN_EVT_WINDOW_OUTPUT;
        ev.window = w;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    case SWP_MSG_LAYOUT_RESULT: {
        swp_layout_result m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        ev.type = STLXWIN_EVT_LAYOUT_RESULT;
        ev.layout_result.outcome = m.outcome;
        ev.layout_result.generation = m.generation;
        stlxwin_evq_push(conn, &ev);
        return;
    }
    default:
        return;
    }
}

/* Pull everything available off the socket, assembling messages across
 * short reads */
int stlxwin_dispatch(stlxwin_conn* conn) {
    if (!conn || conn->disconnect_queued) {
        return -1;
    }

    uint32_t before = conn->evq_count;

    while (!conn->dead) {
        struct iovec iov = { conn->rd_buf + conn->rd_have, sizeof(conn->rd_buf) - conn->rd_have };
        union {
            struct cmsghdr head;
            uint8_t        space[HANDLE_CONTROL_BYTES];
        } control;

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.space;
        msg.msg_controllen = sizeof(control.space);

        ssize_t n = recvmsg(conn->fd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            stlxwin_mark_dead(conn);
            break;
        }

        if (n == 0) {
            stlxwin_mark_dead(conn);
            break;
        }

        queue_received_handles(conn, &msg);
        conn->rd_have += (uint32_t)n;

        /* Consume every complete message in the buffer */
        uint32_t off = 0;
        while (conn->rd_have - off >= sizeof(swp_header)) {
            swp_header hdr;
            memcpy(&hdr, conn->rd_buf + off, sizeof(hdr));
            if (hdr.length > SWP_MAX_MSG_SIZE - sizeof(hdr)) {
                stlxwin_mark_dead(conn);
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
        conn->disconnect_queued = stlxwin_evq_push(conn, &ev) == 0;
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
            return stlxwin_dispatch(conn) > 0 ? 1 : -1;
        }

        struct pollfd pfd = { conn->fd, POLLIN, 0 };
        int timeout_ms = -1;
        if (timeout_ns >= 0) {
            timeout_ms = (int)(timeout_ns / 1000000);
        }

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno != EINTR) {
            stlxwin_mark_dead(conn);
            continue;
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

static const swp_desktop_output* main_output(const stlxwin_conn* conn) {
    for (uint32_t i = 0; i < conn->outputs.count; i++) {
        if ((conn->outputs.outputs[i].flags & SWP_DESKTOP_MAIN) != 0) {
            return &conn->outputs.outputs[i];
        }
    }

    return NULL;
}

void stlxwin_screen_size(const stlxwin_conn* conn, uint32_t* w, uint32_t* h) {
    uint32_t width = 0;
    uint32_t height = 0;
    const swp_desktop_output* main = conn ? main_output(conn) : NULL;
    if (main) {
        width = main->width;
        height = main->height;
    } else if (conn) {
        width = conn->screen_w;
        height = conn->screen_h;
    }

    if (w) {
        *w = width;
    }

    if (h) {
        *h = height;
    }
}

long stlxwin_outputs_get(stlxwin_conn* conn, uint32_t* generation,
                         stlxwin_output* outputs, size_t capacity) {
    const swp_outputs* kept = conn ? &conn->outputs : NULL;
    if (generation) {
        *generation = kept ? kept->generation : 0;
    }

    if (!kept) {
        return 0;
    }

    size_t copied = kept->count < capacity ? kept->count : capacity;
    for (size_t i = 0; i < copied; i++) {
        const swp_desktop_output* in = &kept->outputs[i];
        stlxwin_output* out = &outputs[i];

        out->output = in->output;
        out->flags = in->flags;
        out->x = in->x;
        out->y = in->y;
        out->width = in->width;
        out->height = in->height;

        out->refresh_millihertz = in->refresh_millihertz;
        out->work = (stlxwin_rect){ in->work.x, in->work.y, in->work.w, in->work.h };
        memcpy(out->monitor, in->monitor, sizeof(out->monitor));
        out->monitor[sizeof(out->monitor) - 1] = '\0';
    }

    return (long)kept->count;
}

int stlxwin_layout_set(stlxwin_conn* conn, uint32_t generation, uint32_t main,
                       const stlxwin_layout_entry* entries, uint32_t count) {
    if (!conn || count > SWP_OUTPUTS_MAX) {
        return -1;
    }

    swp_set_layout msg;
    memset(&msg, 0, sizeof(msg));
    msg.generation = generation;
    msg.main = main;
    msg.count = count;
    for (uint32_t i = 0; i < count; i++) {
        msg.entries[i] = (swp_layout_entry){ entries[i].output, entries[i].x, entries[i].y };
    }

    return stlxwin_send(conn, SWP_MSG_SET_LAYOUT, &msg, sizeof(msg));
}

stlxwin_gpu_info stlxwin_gpu_info_get(stlxwin_conn* conn) {
    stlxwin_gpu_info info;
    memset(&info, 0, sizeof(info));
    if (!conn) {
        return info;
    }

    pthread_mutex_lock(&conn->lock);
    info.flags = conn->gpu_info.flags;
    info.device = conn->gpu_info.device;
    pthread_mutex_unlock(&conn->lock);

    return info;
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

    int64_t deadline_ns = 0;
    swp_header hdr = { SWP_MSG_HELLO, 0, sizeof(hello) };
    if (write_all(fd, &hdr, sizeof(hdr), &deadline_ns) != 0 ||
        write_all(fd, &hello, sizeof(hello), &deadline_ns) != 0) {
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

    swp_gpu_info gpu_info;
    memset(&gpu_info, 0, sizeof(gpu_info));
    if ((rhdr.flags & SWP_HELLO_GPU_INFO) && read_hello_gpu_info(fd, &gpu_info) != 0) {
        close(fd);
        return NULL;
    }

    swp_outputs outputs;
    memset(&outputs, 0, sizeof(outputs));
    if ((rhdr.flags & SWP_HELLO_OUTPUTS) && read_hello_outputs(fd, &outputs) != 0) {
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
    pthread_mutex_init(&conn->lock, NULL);
    conn->gpu_fd = -1;
    conn->clip_handle = -1;
    conn->drop_handle = -1;

    conn->fd = fd;
    conn->evq_cap = STLXWIN_EVQ_INITIAL;
    conn->next_win_id = 1;

    conn->screen_w = reply.screen_w;
    conn->screen_h = reply.screen_h;
    conn->gpu_info = gpu_info;
    conn->outputs = outputs;

    return conn;
}

void stlxwin_disconnect(stlxwin_conn* conn) {
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->lock);
    stlxwin_gpu_release_records_locked(conn, NULL);
    pthread_mutex_unlock(&conn->lock);

    while (conn->windows) {
        struct stlxwin_window* w = conn->windows;
        conn->windows = w->next;
        stlxwin_buffers_free_all(w);
        stlxwin_window_free(w);
    }

    close(conn->fd);
    if (conn->gpu_fd >= 0) {
        close(conn->gpu_fd);
    }

    for (uint32_t i = 0; i < conn->queued_handle_count; i++) {
        close(conn->queued_handles[i]);
    }

    if (conn->clip_handle >= 0) {
        close(conn->clip_handle);
    }

    if (conn->drop_handle >= 0) {
        close(conn->drop_handle);
    }

    pthread_mutex_destroy(&conn->lock);
    free(conn->modes);
    free(conn->evq);
    free(conn);
}
