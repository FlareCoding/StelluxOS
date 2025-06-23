#ifndef SYS_GRAPHICS_H
#define SYS_GRAPHICS_H

#include <syscall/syscall_registry.h>

// Graphics framebuffer operations
enum gfx_operations {
    GFX_OP_GET_INFO             = 0x01,
    GFX_OP_MAP_FRAMEBUFFER      = 0x02,
    GFX_OP_UNMAP_FRAMEBUFFER    = 0x03,
    GFX_OP_DISABLE_PREEMPT      = 0x04,
    GFX_OP_ENABLE_PREEMPT       = 0x05
};

// Framebuffer information structure (kernel-side copy)
struct gfx_framebuffer_info {
    uint32_t width;     // Width in pixels
    uint32_t height;    // Height in pixels
    uint32_t pitch;     // Bytes per row
    uint8_t  bpp;       // Bits per pixel
    uint32_t size;      // Total size in bytes
    uint32_t format;    // Pixel format
};

// Declare the graphics framebuffer syscall handler
DECLARE_SYSCALL_HANDLER(gfx_fb_op);

#endif // SYS_GRAPHICS_H
