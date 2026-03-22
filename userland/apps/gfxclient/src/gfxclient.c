#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
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

    uint32_t frame = 0;
    struct timespec ts = { 0, 33000000 };

    while (!stlxgfx_window_should_close(win)) {
        stlxgfx_surface_t* buf = stlxgfx_window_back_buffer(win);
        if (!buf) {
            nanosleep(&ts, NULL);
            continue;
        }

        stlxgfx_clear(buf, 0xFF1A1A2E);

        int32_t rx = (int32_t)(frame % 200);
        stlxgfx_fill_rect(buf, rx, 50, 100, 80, 0xFF4488FF);
        stlxgfx_draw_text(buf, 10, 10, "gfxclient", 14, 0xFFFFFFFF);

        stlxgfx_window_swap_buffers(win);
        frame++;

        nanosleep(&ts, NULL);
    }

    stlxgfx_window_close(win);
    stlxgfx_disconnect(conn);
    printf("gfxclient: closed\r\n");
    return 0;
}
