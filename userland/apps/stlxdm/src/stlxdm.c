#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/fb.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/window.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxgfx_fb_t fb;
    if (stlxgfx_fb_open(&fb) != 0) {
        printf("stlxdm: failed to open framebuffer\r\n");
        return 1;
    }
    printf("stlxdm: framebuffer %ux%u %ubpp\r\n",
           fb.width, fb.height, (unsigned int)fb.bpp);

    stlxgfx_surface_t* backbuf = stlxgfx_fb_create_surface(&fb, fb.width, fb.height);
    if (!backbuf) {
        printf("stlxdm: failed to create back buffer\r\n");
        stlxgfx_fb_close(&fb);
        return 1;
    }

    stlxgfx_dm_window_t* win = stlxgfx_dm_create_window(400, 300, 50, 50, &fb);
    if (!win) {
        printf("stlxdm: failed to create client window\r\n");
        stlxgfx_destroy_surface(backbuf);
        stlxgfx_fb_close(&fb);
        return 1;
    }

    int proc = proc_create("/initrd/bin/gfxclient", NULL);
    if (proc < 0) {
        printf("stlxdm: failed to create gfxclient process\r\n");
    } else {
        proc_set_handle(proc, STLXGFX_WINDOW_SURFACE_FD, win->surface_fd);
        proc_set_handle(proc, STLXGFX_WINDOW_SYNC_FD, win->sync_fd);
        proc_start(proc);
        proc_detach(proc);
        printf("stlxdm: spawned gfxclient\r\n");
    }

    struct timespec ts = { 0, 33000000 };

    while (1) {
        stlxgfx_dm_sync(win);

        stlxgfx_clear(backbuf, 0xFF2D2D30);
        stlxgfx_draw_text(backbuf, 10, 10, "Stellux Display Manager", 0xFFFFFFFF, 0x00000000);

        stlxgfx_surface_t* client_front = stlxgfx_dm_front_buffer(win);
        if (client_front) {
            stlxgfx_blit(backbuf, win->x, win->y,
                         client_front, 0, 0,
                         client_front->width, client_front->height);
        }

        stlxgfx_fb_present(&fb, backbuf);
        nanosleep(&ts, NULL);
    }
}
