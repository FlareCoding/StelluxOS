#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/event.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxgfx_font_init(STLXGFX_FONT_PATH);

    int conn = stlxgfx_connect(STLXGFX_DM_SOCKET_PATH);
    if (conn < 0) {
        printf("gfxclient: failed to connect to DM\r\n");
        return 1;
    }
    printf("gfxclient: connected to DM\r\n");

    stlxgfx_window_t* win = stlxgfx_create_window(conn, 400, 300, "gfxclient");
    if (!win) {
        printf("gfxclient: failed to create window\r\n");
        stlxgfx_disconnect(conn);
        return 1;
    }
    printf("gfxclient: window %u created (%ux%u)\r\n",
           win->window_id, win->width, win->height);

    uint32_t rect_color = 0xFF4488FF;
    int32_t  rect_x = 50;
    int32_t  rect_y = 50;
    struct timespec ts = { 0, 33000000 };

    while (!stlxgfx_window_should_close(win)) {
        stlxgfx_event_t evt;
        while (stlxgfx_window_next_event(win, &evt) == 1) {
            switch (evt.type) {
            case STLXGFX_EVT_KEY_DOWN:
                if (evt.key.usage == 0x29) {
                    goto done;
                }
                rect_color ^= 0x00FF00FF;
                break;
            case STLXGFX_EVT_POINTER_MOVE:
                rect_x = evt.pointer.x - 50;
                rect_y = evt.pointer.y - 40;
                break;
            case STLXGFX_EVT_POINTER_BTN_DOWN:
                rect_color = 0xFFFF4444;
                break;
            case STLXGFX_EVT_POINTER_BTN_UP:
                rect_color = 0xFF4488FF;
                break;
            case STLXGFX_EVT_FOCUS_IN:
                printf("gfxclient: focus in\r\n");
                break;
            case STLXGFX_EVT_FOCUS_OUT:
                printf("gfxclient: focus out\r\n");
                break;
            default:
                break;
            }
        }

        stlxgfx_surface_t* buf = stlxgfx_window_back_buffer(win);
        if (!buf) {
            nanosleep(&ts, NULL);
            continue;
        }

        stlxgfx_clear(buf, 0xFF1A1A2E);
        stlxgfx_fill_rect(buf, rect_x, rect_y, 100, 80, rect_color);
        stlxgfx_draw_text(buf, 10, 10, "gfxclient (input)", 14, 0xFFFFFFFF);

        stlxgfx_window_swap_buffers(win);

        nanosleep(&ts, NULL);
    }

done:
    stlxgfx_window_close(win);
    stlxgfx_disconnect(conn);
    printf("gfxclient: closed\r\n");
    return 0;
}
