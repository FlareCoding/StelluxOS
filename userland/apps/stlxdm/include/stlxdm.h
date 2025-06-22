#ifndef STLXDM_H
#define STLXDM_H

#include <stdint.h>
#include <stlibc/stellux_syscalls.h>

// Graphics framebuffer syscall
#define SYS_GRAPHICS_FRAMEBUFFER_OP 800

// Graphics framebuffer operations
enum gfx_operations {
    GFX_OP_GET_INFO             = 0x01,
    GFX_OP_MAP_FRAMEBUFFER      = 0x02,
    GFX_OP_UNMAP_FRAMEBUFFER    = 0x03
};

// Framebuffer information structure
struct gfx_framebuffer_info {
    uint32_t width;     // Width in pixels
    uint32_t height;    // Height in pixels
    uint32_t pitch;     // Bytes per row
    uint8_t  bpp;       // Bits per pixel
    uint32_t size;      // Total size in bytes
    uint32_t format;    // Pixel format
};

#endif // STLXDM_H
