#include "entry_params.h"
#include <stdint.h>

void set_pixel(int x, int y, int color, struct kernel_entry_params* params) {
    unsigned int* framebuffer = (unsigned int*) params->graphics_framebuffer.base;
    framebuffer[x + (y * params->graphics_framebuffer.width)] = color;
}

void _kentry(struct kernel_entry_params* params) {
    // Draw a colored square in the top left corner
    for (int x = 0; x < 100; ++x) {
        for (int y = 0; y < 100; ++y) {
            set_pixel(x, y, 0xFFEFFFFF, params);
        }
    }

    while (1) {
        __asm__ __volatile__ ("hlt");
    }
}
