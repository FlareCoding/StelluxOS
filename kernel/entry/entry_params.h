#ifndef KERNEL_PARAMS_H
#define KERNEL_PARAMS_H

struct kernel_entry_params {
    void* gop_framebuffer_base;
    void* gop_framebuffer_size;
    unsigned int framebuffer_width;
    unsigned int framebuffer_height;
    unsigned int framebuffer_pixels_per_scanline;
};

#endif // KERNEL_PARAMS_H