#include "entry_params.h"

void set_pixel(int x, int y, int color, struct kernel_entry_params* params) {
    unsigned int* framebuffer = (unsigned int*) params->gop_framebuffer_base;
    framebuffer[x + (y * params->framebuffer_width)] = color;
}

void _kentry(struct kernel_entry_params* params) {
    // Set the first 100x100 pixels to red (0xFF0000 is red in RGB)
    for (int x = 0; x < 100; ++x) {
        for (int y = 0; y < 100; ++y) {
            set_pixel(x, y, 0xFF0000, params);
        }
    }

    while (1) {
        __asm__ __volatile__ ("hlt");
    }
}
