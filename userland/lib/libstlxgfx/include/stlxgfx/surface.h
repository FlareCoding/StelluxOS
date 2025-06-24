#ifndef STLXGFX_SURFACE_H
#define STLXGFX_SURFACE_H

#include <stddef.h>
#include <stdint.h>
#include <stlibc/ipc/shm.h>

typedef struct stlxgfx_context stlxgfx_context_t;

// Forward declaration for window sync structure
typedef struct stlxgfx_window_sync stlxgfx_window_sync_t;

typedef enum {
    STLXGFX_FORMAT_RGB24,   // 24-bit RGB (R,G,B)
    STLXGFX_FORMAT_BGR24,   // 24-bit BGR (B,G,R) - common on GOP
    STLXGFX_FORMAT_ARGB32,  // 32-bit ARGB (A,R,G,B)
    STLXGFX_FORMAT_BGRA32   // 32-bit BGRA (B,G,R,A) - common on GOP
} stlxgfx_pixel_format_t;

typedef struct {
    uint32_t width, height;
    uint32_t pitch;              // bytes per row
    stlxgfx_pixel_format_t format;
    uint8_t pixels[];            // flexible array member - pixel data follows
} stlxgfx_surface_t;

/**
 * Get bits per pixel for a pixel format
 * @param format - pixel format
 * @return bits per pixel, 0 on error
 */
uint8_t stlxgfx_get_bpp_for_format(stlxgfx_pixel_format_t format);

/**
 * Detect GOP pixel format from bits per pixel
 * @param bpp - bits per pixel from GOP
 * @return best matching pixel format
 */
stlxgfx_pixel_format_t stlxgfx_detect_gop_format(uint8_t bpp);

/**
 * Create a surface (Display Manager only)
 * @param ctx - graphics context
 * @param width - surface width in pixels
 * @param height - surface height in pixels
 * @param format - pixel format
 * @return surface pointer or NULL on error
 */
stlxgfx_surface_t* stlxgfx_dm_create_surface(stlxgfx_context_t* ctx,
                                              uint32_t width, uint32_t height,
                                              stlxgfx_pixel_format_t format);

/**
 * Destroy a surface (Display Manager only)
 * @param ctx - graphics context
 * @param surface - surface to destroy
 */
void stlxgfx_dm_destroy_surface(stlxgfx_context_t* ctx, stlxgfx_surface_t* surface);

/**
 * Create a set of three surfaces in shared memory (Display Manager only)
 * @param ctx - graphics context
 * @param width - surface width in pixels
 * @param height - surface height in pixels
 * @param format - pixel format
 * @param out_shm_handle - returns the shared memory handle
 * @param out_surface0 - returns pointer to first surface (16-byte aligned)
 * @param out_surface1 - returns pointer to second surface (16-byte aligned)
 * @param out_surface2 - returns pointer to third surface (16-byte aligned)
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_create_shared_surface_set(stlxgfx_context_t* ctx,
                                         uint32_t width, uint32_t height,
                                         stlxgfx_pixel_format_t format,
                                         shm_handle_t* out_shm_handle,
                                         stlxgfx_surface_t** out_surface0,
                                         stlxgfx_surface_t** out_surface1,
                                         stlxgfx_surface_t** out_surface2);

/**
 * Destroy a shared surface set (Display Manager only)
 * @param ctx - graphics context
 * @param shm_handle - shared memory handle to destroy
 * @param surface0 - pointer to first surface (will be invalidated)
 * @param surface1 - pointer to second surface (will be invalidated)
 * @param surface2 - pointer to third surface (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_destroy_shared_surface_set(stlxgfx_context_t* ctx,
                                          shm_handle_t shm_handle,
                                          stlxgfx_surface_t* surface0,
                                          stlxgfx_surface_t* surface1,
                                          stlxgfx_surface_t* surface2);

/**
 * Map a shared surface set into application address space
 * @param shm_handle - shared memory handle from display manager
 * @param out_surface0 - returns pointer to first surface 
 * @param out_surface1 - returns pointer to second surface
 * @param out_surface2 - returns pointer to third surface
 * @return 0 on success, negative on error
 */
int stlxgfx_map_shared_surface_set(shm_handle_t shm_handle,
                                   stlxgfx_surface_t** out_surface0,
                                   stlxgfx_surface_t** out_surface1,
                                   stlxgfx_surface_t** out_surface2);

/**
 * Unmap a shared surface set from application address space
 * @param shm_handle - shared memory handle
 * @param surface0 - pointer to first surface (will be invalidated)
 * @param surface1 - pointer to second surface (will be invalidated)
 * @param surface2 - pointer to third surface (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_unmap_shared_surface_set(shm_handle_t shm_handle,
                                     stlxgfx_surface_t* surface0,
                                     stlxgfx_surface_t* surface1,
                                     stlxgfx_surface_t* surface2);

/**
 * Create window sync shared memory (Display Manager only)
 * @param ctx - graphics context
 * @param out_shm_handle - returns the shared memory handle
 * @param out_sync - returns pointer to window sync structure
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_create_window_sync_shm(stlxgfx_context_t* ctx,
                                      shm_handle_t* out_shm_handle,
                                      stlxgfx_window_sync_t** out_sync);

/**
 * Destroy window sync shared memory (Display Manager only)
 * @param ctx - graphics context
 * @param shm_handle - shared memory handle to destroy
 * @param sync - pointer to sync structure (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_destroy_window_sync_shm(stlxgfx_context_t* ctx,
                                       shm_handle_t shm_handle,
                                       stlxgfx_window_sync_t* sync);

/**
 * Map window sync shared memory into application address space
 * @param shm_handle - shared memory handle from display manager
 * @param out_sync - returns pointer to window sync structure
 * @return 0 on success, negative on error
 */
int stlxgfx_map_window_sync_shm(shm_handle_t shm_handle,
                                stlxgfx_window_sync_t** out_sync);

/**
 * Unmap window sync shared memory from application address space
 * @param shm_handle - shared memory handle
 * @param sync - pointer to sync structure (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_unmap_window_sync_shm(shm_handle_t shm_handle,
                                  stlxgfx_window_sync_t* sync);

// =========================
// Drawing Primitives
// =========================

/**
 * Draw a single pixel to surface
 * @param surface - target surface
 * @param x, y - pixel coordinates
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_draw_pixel(stlxgfx_surface_t* surface, uint32_t x, uint32_t y, uint32_t color);

/**
 * Clear entire surface with solid color
 * @param surface - target surface
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_clear_surface(stlxgfx_surface_t* surface, uint32_t color);

/**
 * Fill rectangle with solid color
 * @param surface - target surface
 * @param x, y - top-left corner
 * @param width, height - rectangle dimensions
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_fill_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y, 
                      uint32_t width, uint32_t height, uint32_t color);

/**
 * Draw rectangle outline
 * @param surface - target surface
 * @param x, y - top-left corner
 * @param width, height - rectangle dimensions
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_draw_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t color);

/**
 * Fill rounded rectangle with solid color
 * @param surface - target surface
 * @param x, y - top-left corner
 * @param width, height - rectangle dimensions
 * @param radius - corner radius in pixels
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_fill_rounded_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height, uint32_t radius, uint32_t color);

/**
 * Draw rounded rectangle outline
 * @param surface - target surface
 * @param x, y - top-left corner
 * @param width, height - rectangle dimensions
 * @param radius - corner radius in pixels
 * @param color - color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_draw_rounded_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height, uint32_t radius, uint32_t color);

/**
 * Render text to surface using loaded font
 * @param ctx - graphics context (must have font loaded)
 * @param surface - target surface
 * @param text - null-terminated string
 * @param x, y - text position (baseline)
 * @param font_size - font size in pixels
 * @param color - text color in 0xAARRGGBB format
 * @return 0 on success, negative on error
 */
int stlxgfx_render_text(stlxgfx_context_t* ctx, stlxgfx_surface_t* surface,
                        const char* text, uint32_t x, uint32_t y, 
                        uint32_t font_size, uint32_t color);

/**
 * Blit (copy) one surface to another with format conversion
 * @param src - source surface
 * @param src_x, src_y - source position
 * @param dst - destination surface
 * @param dst_x, dst_y - destination position
 * @param width, height - copy dimensions
 * @return 0 on success, negative on error
 */
int stlxgfx_blit_surface(stlxgfx_surface_t* src, uint32_t src_x, uint32_t src_y,
                         stlxgfx_surface_t* dst, uint32_t dst_x, uint32_t dst_y,
                         uint32_t width, uint32_t height);

/**
 * Blit surface directly to memory buffer (same format/dimensions assumed)
 * @param surface - source surface
 * @param buffer - destination buffer
 * @param buffer_pitch - destination buffer pitch in bytes
 * @return 0 on success, negative on error
 */
int stlxgfx_blit_surface_to_buffer(stlxgfx_surface_t* surface, uint8_t* buffer, uint32_t buffer_pitch);

#endif // STLXGFX_SURFACE_H 