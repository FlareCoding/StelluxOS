#ifndef KERNEL_PARAMS_H
#define KERNEL_PARAMS_H
#include "ktypes.h"

struct kernel_entry_params {
    struct {
        void*     base;
        uint64_t  size;
        uint32_t  width;
        uint32_t  height;
        uint32_t  pixels_per_scanline;
    } graphics_framebuffer;

    struct {
        void*     base;
        uint64_t  size;
        uint64_t  descriptor_size;
        uint64_t  descriptor_count;
    } efi_memory_map;
};

#endif // KERNEL_PARAMS_H