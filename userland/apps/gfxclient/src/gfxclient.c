#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxgfx_window_t* win = stlxgfx_window_open(
        STLXGFX_WINDOW_SURFACE_FD, STLXGFX_WINDOW_SYNC_FD);
    if (!win) {
        printf("gfxclient: failed to open window\r\n");
        return 1;
    }
    printf("gfxclient: window opened\r\n");

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
        stlxgfx_draw_text(buf, 10, 10, "gfxclient", 0xFFFFFFFF, 0x00000000);

        stlxgfx_window_present(win);
        frame++;

        nanosleep(&ts, NULL);
    }

    stlxgfx_window_close(win);
    printf("gfxclient: closed\r\n");
    return 0;
}
