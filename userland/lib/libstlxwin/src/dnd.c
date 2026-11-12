#define _GNU_SOURCE
#include <stlxwin/internal/priv.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DND_ACTIONS (STLXWIN_DND_COPY | STLXWIN_DND_MOVE | STLXWIN_DND_LINK)

/* UTF-8 continuation bytes are 10xxxxxx */
#define UTF8_CONTINUATION_MASK 0xC0u
#define UTF8_CONTINUATION      0x80u

static int is_picture_valid(const stlxwin_drag_picture* picture) {
    int is_sized = picture->width >= 1 && picture->width <= STLXWIN_DRAG_PICTURE_MAX && picture->height >= 1 &&
                   picture->height <= STLXWIN_DRAG_PICTURE_MAX;

    return picture->pixels && is_sized && picture->stride >= picture->width * 4;
}

/* The picture's rows packed `width` pixels apart into a shared memory object, as START_DRAG carries it */
static int create_picture(const stlxwin_drag_picture* picture) {
    size_t row_bytes = (size_t)picture->width * 4;
    size_t bytes = row_bytes * picture->height;
    if (picture->stride == row_bytes) {
        return stlxwin_create_shared(picture->pixels, bytes);
    }

    uint8_t* packed = malloc(bytes);
    if (!packed) {
        return -1;
    }

    const uint8_t* rows = (const uint8_t*)picture->pixels;
    for (uint32_t y = 0; y < picture->height; y++) {
        memcpy(packed + y * row_bytes, rows + (size_t)y * picture->stride, row_bytes);
    }

    int object = stlxwin_create_shared(packed, bytes);
    free(packed);

    return object;
}

/* Copies as much of `words` as fits `out` with its NUL, never splitting a character */
static void copy_words(char* out, size_t capacity, const char* words) {
    size_t len = strlen(words);
    if (len >= capacity) {
        len = capacity - 1;
        while (len > 0 && ((unsigned char)words[len] & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION) {
            len--;
        }
    }

    memcpy(out, words, len);
    out[len] = '\0';
}

int stlxwin_drag_start(stlxwin_window* win, uint32_t serial, const stlxwin_offer_rep* reps, uint32_t count,
                       const stlxwin_drag_picture* picture, uint32_t actions, uint32_t item_count) {
    int is_offer_valid = count <= SWP_OFFER_TYPES_MAX && (count == 0 || reps);
    int are_actions_valid = (actions & DND_ACTIONS) != 0 && (actions & ~DND_ACTIONS) == 0;
    if (!win || win->dead || win->conn->dead || serial == 0 || !is_offer_valid || !are_actions_valid ||
        item_count == 0) {
        return -1;
    }

    if (picture && !is_picture_valid(picture)) {
        return -1;
    }

    swp_start_drag msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.serial = serial;
    msg.actions = actions;
    msg.item_count = item_count;

    int handles[SWP_HANDLES_MAX];
    uint32_t handle_count = 0;
    if (picture) {
        int object = create_picture(picture);
        if (object < 0) {
            return -1;
        }

        handles[handle_count++] = object;
        msg.picture_w = picture->width;
        msg.picture_h = picture->height;
        msg.hot_x = picture->hot_x;
        msg.hot_y = picture->hot_y;
    }

    int packed = stlxwin_pack_offer(reps, count, 0, &msg.offer, handles + handle_count);
    if (packed < 0) {
        stlxwin_close_objects(handles, handle_count);
        return -1;
    }

    handle_count += (uint32_t)packed;
    stlxwin_conn* conn = win->conn;
    if (handle_count == 0) {
        return stlxwin_send(conn, SWP_MSG_START_DRAG, &msg, sizeof(msg));
    }

    pthread_mutex_lock(&conn->lock);
    int rc = stlxwin_send_handles_locked(conn, SWP_MSG_START_DRAG, &msg, sizeof(msg), handles, handle_count);
    pthread_mutex_unlock(&conn->lock);
    stlxwin_close_objects(handles, handle_count);

    return rc;
}

int stlxwin_drag_cancel(stlxwin_conn* conn, uint32_t serial) {
    if (!conn || conn->dead) {
        return -1;
    }

    swp_cancel_drag msg = { serial };

    return stlxwin_send(conn, SWP_MSG_CANCEL_DRAG, &msg, sizeof(msg));
}

int stlxwin_drop_status(stlxwin_window* win, uint32_t actions, uint32_t preferred, uint32_t words_action,
                        const char* words) {
    if (!win || win->dead || win->drop_drag == 0 || win->dropped) {
        return -1;
    }

    swp_drop_status msg;
    memset(&msg, 0, sizeof(msg));
    msg.win_id = win->win_id;
    msg.drag = win->drop_drag;

    msg.actions = actions;
    msg.preferred = preferred;
    msg.words_action = words_action;
    if (words) {
        copy_words(msg.words, sizeof(msg.words), words);
    }

    return stlxwin_send(win->conn, SWP_MSG_DROP_STATUS, &msg, sizeof(msg));
}

int stlxwin_drop_types(stlxwin_window* win, stlxwin_offer_types* out) {
    if (!win || !out || win->drop_drag == 0) {
        return -1;
    }

    memcpy(out, &win->drop_types, sizeof(*out));

    return 0;
}

long stlxwin_drop_read(stlxwin_window* win, const char* mime_type, char** out_data) {
    if (!out_data) {
        return -1;
    }

    *out_data = NULL;
    if (!win || win->dead || win->conn->dead || !win->dropped || !mime_type) {
        return -1;
    }

    swp_drop_get ask;
    memset(&ask, 0, sizeof(ask));
    size_t type_len = strlen(mime_type);
    if (type_len == 0 || type_len >= sizeof(ask.mime)) {
        return -1;
    }

    ask.win_id = win->win_id;
    ask.drag = win->drop_drag;
    memcpy(ask.mime, mime_type, type_len);

    /* The ask waits for stlxdm's answer, queueing the events that arrive meanwhile */
    stlxwin_conn* conn = win->conn;
    conn->drop_pending = 1;
    if (stlxwin_send(conn, SWP_MSG_DROP_GET, &ask, sizeof(ask)) != 0) {
        return -1;
    }

    stlxwin_wait_while(conn, &conn->drop_pending, 1);
    if (conn->dead || conn->drop_len == 0) {
        return conn->dead ? -1 : 0;
    }

    int object = conn->drop_handle;
    conn->drop_handle = -1;
    if (object < 0) {
        return -1;
    }

    char* data = stlxwin_copy_shared(object, conn->drop_len);
    close(object);
    if (!data) {
        return -1;
    }

    *out_data = data;

    return (long)conn->drop_len;
}

int stlxwin_drop_finish(stlxwin_window* win, uint32_t action, uint32_t flags) {
    if (!win || win->dead || !win->dropped) {
        return -1;
    }

    swp_drop_finish msg = { win->win_id, win->drop_drag, action, flags };
    win->drop_drag = 0;
    win->dropped = 0;
    memset(&win->drop_types, 0, sizeof(win->drop_types));

    return stlxwin_send(win->conn, SWP_MSG_DROP_FINISH, &msg, sizeof(msg));
}

/* A drop event of `type` at the window position a message names */
static stlxwin_event drop_event(stlxwin_event_type type, struct stlxwin_window* w, int32_t x, int32_t y,
                                uint32_t drag, uint32_t modifiers) {
    stlxwin_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.window = w;

    ev.drop.x = x;
    ev.drop.y = y;
    ev.drop.drag = drag;
    ev.drop.modifiers = (uint8_t)modifiers;

    return ev;
}

static void take_drag_state(stlxwin_conn* conn, const swp_drag_state* m) {
    struct stlxwin_window* w = stlxwin_find_window(conn, m->win_id);
    if (!w) {
        return;
    }

    stlxwin_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = STLXWIN_EVT_DRAG_STATE;
    ev.window = w;

    ev.drag_state.drag = m->drag;
    ev.drag_state.state = m->state;
    ev.drag_state.action = m->action;
    stlxwin_evq_push(conn, &ev);
}

/* A new drag over the window replaces whatever drag it held */
static void take_drop_enter(stlxwin_conn* conn, const swp_drop_enter* m) {
    struct stlxwin_window* w = stlxwin_find_window(conn, m->win_id);
    if (!w || m->drag == 0 || m->count > SWP_OFFER_TYPES_MAX) {
        return;
    }

    w->drop_drag = m->drag;
    w->dropped = 0;
    memset(&w->drop_types, 0, sizeof(w->drop_types));
    w->drop_types.count = m->count;
    for (uint32_t i = 0; i < m->count; i++) {
        memcpy(w->drop_types.types[i], m->types[i], SWP_MIME_MAX - 1);
    }

    stlxwin_event ev = drop_event(STLXWIN_EVT_DROP_ENTER, w, m->x, m->y, m->drag, m->modifiers);
    ev.drop.actions = m->actions;
    ev.drop.item_count = m->item_count;
    ev.drop.flags = m->flags;
    stlxwin_evq_push(conn, &ev);
}

/* The window holding the drag a message names, or NULL once it left or was dropped elsewhere */
static struct stlxwin_window* drop_window(stlxwin_conn* conn, uint32_t win_id, uint32_t drag) {
    struct stlxwin_window* w = stlxwin_find_window(conn, win_id);
    return w && w->drop_drag == drag ? w : NULL;
}

static void take_drop_motion(stlxwin_conn* conn, const swp_drop_motion* m) {
    struct stlxwin_window* w = drop_window(conn, m->win_id, m->drag);
    if (!w || w->dropped) {
        return;
    }

    stlxwin_event ev = drop_event(STLXWIN_EVT_DROP_MOTION, w, m->x, m->y, m->drag, m->modifiers);
    stlxwin_evq_push(conn, &ev);
}

static void take_drop_leave(stlxwin_conn* conn, const swp_drop_leave* m) {
    struct stlxwin_window* w = drop_window(conn, m->win_id, m->drag);
    if (!w || w->dropped) {
        return;
    }

    w->drop_drag = 0;
    memset(&w->drop_types, 0, sizeof(w->drop_types));

    stlxwin_event ev = drop_event(STLXWIN_EVT_DROP_LEAVE, w, 0, 0, m->drag, 0);
    stlxwin_evq_push(conn, &ev);
}

static void take_drop(stlxwin_conn* conn, const swp_drop* m) {
    struct stlxwin_window* w = drop_window(conn, m->win_id, m->drag);
    if (!w || w->dropped) {
        return;
    }

    w->dropped = 1;

    stlxwin_event ev = drop_event(STLXWIN_EVT_DROP, w, m->x, m->y, m->drag, m->modifiers);
    ev.drop.action = m->action;
    stlxwin_evq_push(conn, &ev);
}

/* A payload may sit at any offset of the read buffer, so each is copied rather than cast */
void stlxwin_dnd_take_message(stlxwin_conn* conn, const swp_header* hdr, const uint8_t* payload) {
    switch (hdr->type) {
    case SWP_MSG_DRAG_STATE: {
        swp_drag_state m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        take_drag_state(conn, &m);
        return;
    }
    case SWP_MSG_DROP_ENTER: {
        swp_drop_enter m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        take_drop_enter(conn, &m);
        return;
    }
    case SWP_MSG_DROP_MOTION: {
        swp_drop_motion m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        take_drop_motion(conn, &m);
        return;
    }
    case SWP_MSG_DROP_LEAVE: {
        swp_drop_leave m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        take_drop_leave(conn, &m);
        return;
    }
    case SWP_MSG_DROP: {
        swp_drop m;
        if (hdr->length != sizeof(m)) {
            return;
        }

        memcpy(&m, payload, sizeof(m));
        take_drop(conn, &m);
        return;
    }
    default:
        return;
    }
}
