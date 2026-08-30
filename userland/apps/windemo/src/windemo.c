/* windemo: the smallest protocol client, used to exercise the window
 * stack end to end. Draws a checker pattern, reports the first frame
 * presentation, and repaints on resize.
 */
#include <stlxwin/stlxwin.h>

#include <stdio.h>

static void paint(stlxwin_buffer* buf) {
    for (uint32_t y = 0; y < buf->height; y++) {
        uint32_t* row = buf->pixels + y * (buf->stride / 4);
        for (uint32_t x = 0; x < buf->width; x++) {
            row[x] = ((x / 16) ^ (y / 16)) & 1 ? 0x00446688 : 0x0089B4FA;
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxwin_conn* conn = stlxwin_connect("windemo");
    if (!conn) {
        printf("windemo: no display manager\r\n");
        return 1;
    }

    stlxwin_window* win = stlxwin_window_create(conn, 400, 300,
                                                "Window Demo",
                                                STLXWIN_WF_RESIZABLE);
    if (!win) {
        printf("windemo: window creation failed\r\n");
        stlxwin_disconnect(conn);
        return 1;
    }

    stlxwin_buffer* buf = stlxwin_begin_frame(win);
    if (!buf) {
        stlxwin_disconnect(conn);
        return 1;
    }

    paint(buf);
    stlxwin_commit(win, buf, 0, 0, STLXWIN_COMMIT_WANT_FRAME);
    printf("windemo: first frame committed\r\n");

    int running = 1;
    while (running) {
        if (stlxwin_wait(conn, -1) < 0) {
            break;
        }

        stlxwin_event ev;
        while (stlxwin_next_event(conn, &ev)) {
            switch (ev.type) {
            case STLXWIN_EVT_FRAME:
                printf("windemo: frame presented\r\n");
                break;
            case STLXWIN_EVT_CONFIGURE: {
                stlxwin_buffer* b = stlxwin_begin_frame(win);
                if (b) {
                    paint(b);
                    stlxwin_commit(win, b, 0, 0, 0);
                }
                break;
            }
            case STLXWIN_EVT_CLOSE:
            case STLXWIN_EVT_DISCONNECTED:
                running = 0;
                break;
            default:
                break;
            }
        }
    }

    stlxwin_window_destroy(win);
    stlxwin_disconnect(conn);
    return 0;
}
