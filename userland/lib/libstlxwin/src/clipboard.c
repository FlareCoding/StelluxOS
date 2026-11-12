#define _GNU_SOURCE
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* A shared memory object holding the bytes, which the display manager keeps as part of the clipboard */
static int create_shared(const void* data, size_t len) {
    int object = memfd_create("stlxwin-clipboard", MFD_CLOEXEC);
    if (object < 0) {
        return -1;
    }

    if (ftruncate(object, (off_t)len) != 0) {
        close(object);
        return -1;
    }

    void* shared = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, object, 0);
    if (shared == MAP_FAILED) {
        close(object);
        return -1;
    }

    memcpy(shared, data, len);
    munmap(shared, len);

    return object;
}

/* Fails when another client shrank the object below len, which the mapping refuses */
static char* copy_shared(int object, uint64_t len) {
    void* shared = mmap(NULL, len, PROT_READ, MAP_SHARED, object, 0);
    if (shared == MAP_FAILED) {
        return NULL;
    }

    char* data = malloc(len + 1);
    if (data) {
        memcpy(data, shared, len);
        data[len] = '\0';
    }

    munmap(shared, len);

    return data;
}

static void close_objects(const int* objects, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        close(objects[i]);
    }
}

/* Reads the connection while `*flag` holds `value`, queueing the events that arrive meanwhile for the app to drain
 * afterwards */
static void wait_while(stlxwin_conn* conn, const int* flag, int value) {
    while (*flag == value && !conn->dead) {
        struct pollfd pfd = { conn->fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, -1);
        if (rc < 0 && errno != EINTR) {
            stlxwin_mark_dead(conn);
            break;
        }

        if (rc > 0) {
            stlxwin_dispatch(conn);
        }
    }
}

/* Asks with an untyped or a typed CLIPBOARD_GET and copies the representation it answers with */
static long fetch(stlxwin_conn* conn, const void* ask, uint32_t ask_len, char** out_data) {
    *out_data = NULL;
    conn->clip_pending = 1;
    if (stlxwin_send(conn, SWP_MSG_CLIPBOARD_GET, ask, ask_len) != 0) {
        return -1;
    }

    wait_while(conn, &conn->clip_pending, 1);
    if (conn->dead || conn->clip_len == 0) {
        return conn->dead ? -1 : 0;
    }

    /* Closed once copied, so no reader keeps a replaced clipboard's memory alive */
    int object = conn->clip_handle;
    conn->clip_handle = -1;
    if (object < 0) {
        return -1;
    }

    char* data = copy_shared(object, conn->clip_len);
    close(object);
    if (!data) {
        return -1;
    }

    *out_data = data;

    return (long)conn->clip_len;
}

int stlxwin_clipboard_set(stlxwin_conn* conn, const char* text, size_t len) {
    if (!conn || conn->dead || !text) {
        return -1;
    }

    swp_clipboard_set msg = { len };
    if (len == 0) {
        return stlxwin_send(conn, SWP_MSG_CLIPBOARD_SET, &msg, sizeof(msg));
    }

    int object = create_shared(text, len);
    if (object < 0) {
        return -1;
    }

    pthread_mutex_lock(&conn->lock);
    int rc = stlxwin_send_handles_locked(conn, SWP_MSG_CLIPBOARD_SET, &msg, sizeof(msg), &object, 1);
    pthread_mutex_unlock(&conn->lock);
    close(object);

    return rc;
}

long stlxwin_clipboard_get(stlxwin_conn* conn, char** out_text) {
    if (!conn || conn->dead || !out_text) {
        return -1;
    }

    return fetch(conn, NULL, 0, out_text);
}

int stlxwin_clipboard_offer(stlxwin_conn* conn, const stlxwin_clip_rep* reps, uint32_t count, uint32_t flags) {
    if (!conn || conn->dead || count > SWP_OFFER_TYPES_MAX || (count > 0 && !reps)) {
        return -1;
    }

    swp_offer msg;
    memset(&msg, 0, sizeof(msg));
    msg.count = count;
    msg.flags = flags;

    /* Each representation with bytes goes into a shared memory object of its own, whose handle follows in order */
    int objects[SWP_OFFER_TYPES_MAX];
    uint32_t object_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t type_len = reps[i].mime_type ? strlen(reps[i].mime_type) : 0;
        if (type_len == 0 || type_len >= SWP_MIME_MAX || (reps[i].len > 0 && !reps[i].data)) {
            close_objects(objects, object_count);
            return -1;
        }

        memcpy(msg.types[i].mime, reps[i].mime_type, type_len);
        msg.types[i].len = reps[i].len;
        if (reps[i].len == 0) {
            continue;
        }

        int object = create_shared(reps[i].data, reps[i].len);
        if (object < 0) {
            close_objects(objects, object_count);
            return -1;
        }

        objects[object_count++] = object;
    }

    if (object_count == 0) {
        return stlxwin_send(conn, SWP_MSG_CLIPBOARD_OFFER, &msg, sizeof(msg));
    }

    pthread_mutex_lock(&conn->lock);
    int rc = stlxwin_send_handles_locked(conn, SWP_MSG_CLIPBOARD_OFFER, &msg, sizeof(msg), objects, object_count);
    pthread_mutex_unlock(&conn->lock);
    close_objects(objects, object_count);

    return rc;
}

long stlxwin_clipboard_get_type(stlxwin_conn* conn, const char* mime_type, char** out_data) {
    if (!conn || conn->dead || !mime_type || !out_data) {
        return -1;
    }

    swp_clipboard_get ask;
    memset(&ask, 0, sizeof(ask));
    size_t type_len = strlen(mime_type);
    if (type_len == 0 || type_len >= sizeof(ask.mime)) {
        return -1;
    }

    memcpy(ask.mime, mime_type, type_len);

    return fetch(conn, &ask, sizeof(ask), out_data);
}

int stlxwin_clipboard_watch(stlxwin_conn* conn) {
    if (!conn || conn->dead) {
        return -1;
    }

    if (conn->has_clip_types) {
        return 0;
    }

    if (stlxwin_send(conn, SWP_MSG_CLIPBOARD_WATCH, NULL, 0) != 0) {
        return -1;
    }

    wait_while(conn, &conn->has_clip_types, 0);

    return conn->has_clip_types ? 0 : -1;
}

int stlxwin_clipboard_types(stlxwin_conn* conn, stlxwin_clip_types* out) {
    if (!conn || !out || !conn->has_clip_types) {
        return -1;
    }

    memcpy(out, &conn->clip_types, sizeof(*out));

    return 0;
}
