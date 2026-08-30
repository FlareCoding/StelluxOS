/* windemo: the smallest protocol client, used to exercise the window
 * stack end to end. Draws a checker pattern, reports the first frame
 * presentation, and repaints on resize.
 */
#include <stlxwin/stlxwin.h>

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void paint_colored(stlxwin_buffer* buf, uint32_t a, uint32_t b) {
    for (uint32_t y = 0; y < buf->height; y++) {
        uint32_t* row = buf->pixels + y * (buf->stride / 4);
        for (uint32_t x = 0; x < buf->width; x++) {
            row[x] = ((x / 16) ^ (y / 16)) & 1 ? a : b;
        }
    }
}

static void paint(stlxwin_buffer* buf) {
    paint_colored(buf, 0x00446688, 0x0089B4FA);
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

    stlxwin_window* win2 = NULL;
    stlxwin_window* popup = NULL;

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
            case STLXWIN_EVT_KEY_DOWN:
            case STLXWIN_EVT_KEY_REPEAT:
                /* s stalls the client to exercise outbound queueing */
                if (ev.key.ch == 's') {
                    printf("windemo: stalling 3s\r\n");
                    struct timespec ts = { 3, 0 };
                    nanosleep(&ts, NULL);
                    printf("windemo: awake\r\n");
                    break;
                }
                /* w maps a second overlapping window, p opens a
                 * grabbing popup on the first window */
                if (ev.key.ch == 'w' && !win2) {
                    win2 = stlxwin_window_create(conn, 400, 300,
                                                 "Second", 0);
                    stlxwin_buffer* b = win2
                        ? stlxwin_begin_frame(win2) : NULL;
                    if (b) {
                        paint_colored(b, 0x00336644, 0x0066CC88);
                        stlxwin_commit(win2, b, 0, 0, 0);
                        printf("windemo: second window mapped\r\n");
                    }
                    break;
                }
                if (ev.key.ch == 'p' && !popup) {
                    popup = stlxwin_popup_create(win, 300, 50, 200, 150,
                                                 STLXWIN_PF_GRAB);
                    stlxwin_buffer* b = popup
                        ? stlxwin_begin_frame(popup) : NULL;
                    if (b) {
                        paint_colored(b, 0x00AA5577, 0x00F38BA8);
                        stlxwin_commit(popup, b, 0, 0, 0);
                        printf("windemo: popup mapped\r\n");
                    }
                    break;
                }
                /* d commits a small patch with exact damage */
                if (ev.key.ch == 'd') {
                    stlxwin_buffer* b = stlxwin_begin_frame(win);
                    if (b) {
                        static uint32_t step = 0;
                        int32_t px = (int32_t)((step * 50) % (b->width - 40));
                        step++;
                        paint(b);
                        for (uint32_t y = 0; y < 40; y++) {
                            uint32_t* row = b->pixels
                                          + (100 + y) * (b->stride / 4);
                            for (uint32_t x = 0; x < 40; x++) {
                                row[(uint32_t)px + x] = 0x00F38BA8;
                            }
                        }
                        stlxwin_rect r = { px, 100, 40, 40 };
                        stlxwin_commit(win, b, &r, 1, 0);
                        printf("windemo: patch at %d\r\n", px);
                    }
                    break;
                }
                /* c and v exercise the clipboard round trip */
                if (ev.key.ch == 'c') {
                    stlxwin_clipboard_set(conn, "windemo clip", 12);
                    printf("windemo: clipboard set\r\n");
                    break;
                }
                if (ev.key.ch == 'v') {
                    char* text = NULL;
                    long n = stlxwin_clipboard_get(conn, &text);
                    printf("windemo: clipboard get %ld '%s'\r\n",
                           n, text ? text : "");
                    free(text);
                    break;
                }
                if (ev.key.ch >= 32 && ev.key.ch < 127) {
                    printf("windemo: key%s '%c' mods=%u\r\n",
                           ev.type == STLXWIN_EVT_KEY_REPEAT ? " repeat" : "",
                           (char)ev.key.ch, ev.key.modifiers);
                } else {
                    printf("windemo: key%s usage=0x%x mods=%u\r\n",
                           ev.type == STLXWIN_EVT_KEY_REPEAT ? " repeat" : "",
                           ev.key.usage, ev.key.modifiers);
                }
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
                printf("windemo: button %u down at %d,%d\r\n",
                       ev.button.button, ev.button.x, ev.button.y);
                break;
            case STLXWIN_EVT_POPUP_DISMISSED:
                printf("windemo: popup dismissed\r\n");
                if (popup) {
                    stlxwin_window_destroy(popup);
                    popup = NULL;
                }
                break;
            case STLXWIN_EVT_POINTER_ENTER:
                printf("windemo: pointer enter at %d,%d\r\n",
                       ev.motion.x, ev.motion.y);
                break;
            case STLXWIN_EVT_FOCUS_IN:
                printf("windemo: focused\r\n");
                break;
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
