#define _GNU_SOURCE
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* A shared memory object holding the text, which the display manager keeps as the clipboard */
static int create_shared_text(const char* text, size_t len) {
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

    memcpy(shared, text, len);
    munmap(shared, len);

    return object;
}

/* Fails when another client shrank the object below len, which the mapping refuses */
static char* copy_shared_text(int object, uint64_t len) {
    void* shared = mmap(NULL, len, PROT_READ, MAP_SHARED, object, 0);
    if (shared == MAP_FAILED) {
        return NULL;
    }

    char* text = malloc(len + 1);
    if (text) {
        memcpy(text, shared, len);
        text[len] = '\0';
    }

    munmap(shared, len);

    return text;
}

int stlxwin_clipboard_set(stlxwin_conn* conn, const char* text, size_t len) {
    if (!conn || conn->dead || !text) {
        return -1;
    }

    swp_clipboard_set msg = { len };
    if (len == 0) {
        return stlxwin_send(conn, SWP_MSG_CLIPBOARD_SET, &msg, sizeof(msg));
    }

    int object = create_shared_text(text, len);
    if (object < 0) {
        return -1;
    }

    pthread_mutex_lock(&conn->lock);
    int rc = stlxwin_send_handles_locked(conn, SWP_MSG_CLIPBOARD_SET, &msg, sizeof(msg), &object, 1);
    pthread_mutex_unlock(&conn->lock);
    close(object);

    return rc;
}

/* Synchronous fetch. Events arriving while waiting are queued for the
 * app to drain afterwards. */
long stlxwin_clipboard_get(stlxwin_conn* conn, char** out_text) {
    if (!conn || conn->dead || !out_text) {
        return -1;
    }

    *out_text = NULL;
    conn->clip_pending = 1;
    if (stlxwin_send(conn, SWP_MSG_CLIPBOARD_GET, NULL, 0) != 0) {
        return -1;
    }

    while (conn->clip_pending && !conn->dead) {
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

    if (conn->dead || conn->clip_len == 0) {
        return conn->dead ? -1 : 0;
    }

    /* Closed once copied, so no reader keeps a replaced clipboard's memory alive */
    int object = conn->clip_handle;
    conn->clip_handle = -1;
    if (object < 0) {
        return -1;
    }

    char* text = copy_shared_text(object, conn->clip_len);
    close(object);
    if (!text) {
        return -1;
    }

    *out_text = text;

    return (long)conn->clip_len;
}
