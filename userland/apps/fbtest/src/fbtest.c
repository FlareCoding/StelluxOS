#include <stdio.h>
#include <stlxgfx/fb.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>

int main(void) {
    stlxgfx_fb_t fb;
    if (stlxgfx_fb_open(&fb) != 0) {
        printf("fbtest: failed to open framebuffer\n");
        return 1;
    }

    printf("fbtest: %ux%u %ubpp\n", fb.width, fb.height, (unsigned int)fb.bpp);

    stlxgfx_surface_t* backbuf = stlxgfx_fb_create_surface(&fb, fb.width, fb.height);
    if (!backbuf) {
        printf("fbtest: failed to create back buffer\n");
        stlxgfx_fb_close(&fb);
        return 1;
    }

    stlxgfx_clear(backbuf, 0xFF1A1A2E);
    stlxgfx_fill_rect(backbuf, 100, 100, 200, 200, 0xFFFFFFFF);
    stlxgfx_draw_rect(backbuf, 95, 95, 210, 210, 0xFF00FF00);
    stlxgfx_draw_text(backbuf, 110, 310, "Hello from libstlxgfx!", 0xFFFFFFFF, 0xFF1A1A2E);

    stlxgfx_fb_present(&fb, backbuf);
    printf("fbtest: rendered to framebuffer\n");

    stlxgfx_destroy_surface(backbuf);
    stlxgfx_fb_close(&fb);
    return 0;
}
