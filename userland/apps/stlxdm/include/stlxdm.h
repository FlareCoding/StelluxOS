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
    GFX_OP_UNMAP_FRAMEBUFFER    = 0x03,
    GFX_OP_DISABLE_PREEMPT      = 0x04,
    GFX_OP_ENABLE_PREEMPT       = 0x05
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

// =========================
// Framebuffer Functions
// =========================

/**
 * Get framebuffer information from the kernel
 * @param fb_info - pointer to structure to fill with framebuffer info
 * @return 0 on success, negative on error
 */
int stlxdm_get_framebuffer_info(struct gfx_framebuffer_info* fb_info);

/**
 * Map the framebuffer into userspace
 * @return pointer to mapped framebuffer on success, NULL on error
 */
uint8_t* stlxdm_map_framebuffer(void);

/**
 * Unmap the framebuffer from userspace
 * @return 0 on success, negative on error
 */
int stlxdm_unmap_framebuffer(void);

/**
 * Begin frame rendering - disables preemption
 * @return 0 on success, negative on error
 */
int stlxdm_begin_frame(void);

/**
 * End frame rendering - re-enables preemption
 * @return 0 on success, negative on error
 */
int stlxdm_end_frame(void);

#endif // STLXDM_H
