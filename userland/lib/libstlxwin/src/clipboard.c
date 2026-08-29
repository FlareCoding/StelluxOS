#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

int stlxwin_clipboard_set(stlxwin_conn* conn, const char* text, size_t len) {
    if (!conn || conn->dead || !text || len > SWP_CLIPBOARD_MAX) {
        return -1;
    }

    uint8_t msg[sizeof(swp_clipboard_set) + SWP_CLIPBOARD_MAX];
    swp_clipboard_set prefix = { (uint32_t)len };
    memcpy(msg, &prefix, sizeof(prefix));
    memcpy(msg + sizeof(prefix), text, len);

    return stlxwin_send(conn, SWP_MSG_CLIPBOARD_SET, msg,
                        (uint32_t)(sizeof(prefix) + len));
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
            conn->dead = 1;
            break;
        }

        if (rc > 0) {
            stlxwin_dispatch(conn);
        }
    }

    if (conn->dead || conn->clip_len == 0) {
        return conn->dead ? -1 : 0;
    }

    char* copy = malloc(conn->clip_len + 1);
    if (!copy) {
        return -1;
    }

    memcpy(copy, conn->clip_text, conn->clip_len);
    copy[conn->clip_len] = '\0';
    *out_text = copy;

    return (long)conn->clip_len;
}
