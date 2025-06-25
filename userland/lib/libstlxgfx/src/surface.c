#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/surface.h"
#include "stlxgfx/internal/stlxgfx_ctx.h"
#include "stlxgfx/internal/stlxgfx_protocol.h"

static inline void write_pixel_to_buffer(uint8_t* pixel, stlxgfx_pixel_format_t format, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    uint8_t a = (color >> 24) & 0xFF;
    
    switch (format) {
        case STLXGFX_FORMAT_RGB24:
            pixel[0] = r; pixel[1] = g; pixel[2] = b;
            break;
        case STLXGFX_FORMAT_BGR24:
            pixel[0] = b; pixel[1] = g; pixel[2] = r;
            break;
        case STLXGFX_FORMAT_ARGB32:
            pixel[0] = a; pixel[1] = r; pixel[2] = g; pixel[3] = b;
            break;
        case STLXGFX_FORMAT_BGRA32:
            pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = a;
            break;
    }
}

static inline uint32_t read_pixel_from_buffer(const uint8_t* pixel, stlxgfx_pixel_format_t format) {
    switch (format) {
        case STLXGFX_FORMAT_RGB24:
            return 0xFF000000 | (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
        case STLXGFX_FORMAT_BGR24:
            return 0xFF000000 | (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
        case STLXGFX_FORMAT_ARGB32:
            return (pixel[0] << 24) | (pixel[1] << 16) | (pixel[2] << 8) | pixel[3];
        case STLXGFX_FORMAT_BGRA32:
            return (pixel[3] << 24) | (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
    }
    return 0;
}

static inline void alpha_blend_pixel(uint8_t* dst_pixel, stlxgfx_pixel_format_t format, uint32_t src_color) {
    uint32_t dst_color = read_pixel_from_buffer(dst_pixel, format);
    
    uint8_t src_a = (src_color >> 24) & 0xFF;
    if (src_a == 0) return; // Fully transparent
    if (src_a == 255) { // Fully opaque
        write_pixel_to_buffer(dst_pixel, format, src_color);
        return;
    }
    
    // Alpha blending: dst = src * src_a + dst * (1 - src_a)
    uint8_t src_r = (src_color >> 16) & 0xFF;
    uint8_t src_g = (src_color >> 8) & 0xFF;
    uint8_t src_b = src_color & 0xFF;
    
    uint8_t dst_r = (dst_color >> 16) & 0xFF;
    uint8_t dst_g = (dst_color >> 8) & 0xFF;
    uint8_t dst_b = dst_color & 0xFF;
    uint8_t dst_a = (dst_color >> 24) & 0xFF;
    
    uint8_t inv_src_a = 255 - src_a;
    uint8_t final_r = (src_r * src_a + dst_r * inv_src_a) / 255;
    uint8_t final_g = (src_g * src_a + dst_g * inv_src_a) / 255;
    uint8_t final_b = (src_b * src_a + dst_b * inv_src_a) / 255;
    uint8_t final_a = src_a + (dst_a * inv_src_a) / 255;
    
    uint32_t final_color = (final_a << 24) | (final_r << 16) | (final_g << 8) | final_b;
    write_pixel_to_buffer(dst_pixel, format, final_color);
}

uint8_t stlxgfx_get_bpp_for_format(stlxgfx_pixel_format_t format) {
    switch (format) {
        case STLXGFX_FORMAT_RGB24:
        case STLXGFX_FORMAT_BGR24:
            return 24;
        case STLXGFX_FORMAT_ARGB32:
        case STLXGFX_FORMAT_BGRA32:
            return 32;
        default:
            return 0; // error
    }
}

stlxgfx_pixel_format_t stlxgfx_detect_gop_format(uint8_t bpp) {
    switch (bpp) {
        case 24: 
            return STLXGFX_FORMAT_BGR24;  // Common on QEMU
        case 32: 
            return STLXGFX_FORMAT_BGRA32; // Common on real hardware
        default: 
            printf("STLXGFX: Unknown GOP BPP %u, defaulting to BGRA32\n", bpp);
            return STLXGFX_FORMAT_BGRA32; // Safe default
    }
}

stlxgfx_surface_t* stlxgfx_dm_create_surface(stlxgfx_context_t* ctx,
                                              uint32_t width, uint32_t height,
                                              stlxgfx_pixel_format_t format) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Surface creation only available in Display Manager mode\n");
        return NULL;
    }
    
    if (width == 0 || height == 0) {
        printf("STLXGFX: Invalid surface dimensions %ux%u\n", width, height);
        return NULL;
    }
    
    uint8_t bpp = stlxgfx_get_bpp_for_format(format);
    if (bpp == 0) {
        printf("STLXGFX: Invalid pixel format %d\n", format);
        return NULL;
    }
    
    // Calculate memory requirements
    uint32_t pitch = width * (bpp / 8);
    size_t pixel_data_size = height * pitch;
    size_t total_size = sizeof(stlxgfx_surface_t) + pixel_data_size;
    
    // Round up to page boundary
    const size_t page_size = STLXGFX_PAGE_SIZE;
    size_t aligned_size = (total_size + page_size - 1) & ~(page_size - 1);
    
    // Allocate page-aligned memory
    stlxgfx_surface_t* surface = malloc(aligned_size);
    if (!surface) {
        printf("STLXGFX: Failed to allocate %zu bytes for surface\n", aligned_size);
        return NULL;
    }
    
    // Initialize surface metadata
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    surface->format = format;
    
    // Clear pixel data to black
    memset(surface->pixels, 0, pixel_data_size);
    
    return surface;
}

void stlxgfx_dm_destroy_surface(stlxgfx_context_t* ctx, stlxgfx_surface_t* surface) {
    if (!ctx || !surface) {
        return;
    }
    
    if (ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Surface destruction only available in Display Manager mode\n");
        return;
    }
    
    free(surface);
}
int stlxgfx_dm_create_shared_surface_set(stlxgfx_context_t* ctx,
                                         uint32_t width, uint32_t height,
                                         stlxgfx_pixel_format_t format,
                                         shm_handle_t* out_shm_handle,
                                         stlxgfx_surface_t** out_surface0,
                                         stlxgfx_surface_t** out_surface1,
                                         stlxgfx_surface_t** out_surface2) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Shared surface set creation only available in Display Manager mode\n");
        return -1;
    }
    
    if (!out_shm_handle || !out_surface0 || !out_surface1 || !out_surface2) {
        printf("STLXGFX: Invalid output parameters for shared surface set\n");
        return -1;
    }
    
    if (width == 0 || height == 0) {
        printf("STLXGFX: Invalid surface dimensions %ux%u\n", width, height);
        return -1;
    }
    
    uint8_t bpp = stlxgfx_get_bpp_for_format(format);
    if (bpp == 0) {
        printf("STLXGFX: Invalid pixel format %d\n", format);
        return -1;
    }
    
    // Calculate memory requirements for one surface
    uint32_t pitch = width * (bpp / 8);
    size_t pixel_data_size = height * pitch;
    size_t single_surface_size = sizeof(stlxgfx_surface_t) + pixel_data_size;
    
    // Round up to 16-byte alignment
    size_t aligned_surface_size = (single_surface_size + 15) & ~15;
    size_t total_size = aligned_surface_size * 3;  // Three aligned surfaces
    
    // Create shared memory
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "stlxgfx_surfaces3_%u_%u", width, height);
    
    shm_handle_t shm_handle = stlx_shm_create(shm_name, total_size, SHM_READ_WRITE);
    if (shm_handle == 0) {
        printf("STLXGFX: Failed to create shared memory for surface set (%zu bytes)\n", total_size);
        return -1;
    }
    
    // Map shared memory
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map shared memory for surface set\n");
        stlx_shm_destroy(shm_handle);
        return -1;
    }
    
    // Initialize first surface (16-byte aligned)
    stlxgfx_surface_t* surface0 = (stlxgfx_surface_t*)shm_memory;
    surface0->width = width;
    surface0->height = height;
    surface0->pitch = pitch;
    surface0->format = format;
    
    // Initialize second surface (16-byte aligned)
    stlxgfx_surface_t* surface1 = (stlxgfx_surface_t*)((uint8_t*)shm_memory + aligned_surface_size);
    surface1->width = width;
    surface1->height = height;
    surface1->pitch = pitch;
    surface1->format = format;
    
    // Initialize third surface (16-byte aligned)
    stlxgfx_surface_t* surface2 = (stlxgfx_surface_t*)((uint8_t*)shm_memory + aligned_surface_size * 2);
    surface2->width = width;
    surface2->height = height;
    surface2->pitch = pitch;
    surface2->format = format;
    
    // Clear all three surfaces to black
    memset(surface0->pixels, 0, pixel_data_size);
    memset(surface1->pixels, 0, pixel_data_size);
    memset(surface2->pixels, 0, pixel_data_size);
    
    // Return results
    *out_shm_handle = shm_handle;
    *out_surface0 = surface0;
    *out_surface1 = surface1;
    *out_surface2 = surface2;
    
    return 0;
}

int stlxgfx_dm_destroy_shared_surface_set(stlxgfx_context_t* ctx,
                                          shm_handle_t shm_handle,
                                          stlxgfx_surface_t* surface0,
                                          stlxgfx_surface_t* surface1,
                                          stlxgfx_surface_t* surface2) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Shared surface set destruction only available in Display Manager mode\n");
        return -1;
    }
    
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory (this invalidates surface pointers)
    if (surface0) {
        // Use surface0 as base address since it's at the start of the SHM region
        if (stlx_shm_unmap(shm_handle, surface0) != 0) {
            printf("STLXGFX: Warning: Failed to unmap shared memory for surface set\n");
            // Continue with destruction attempt
        }
    }
    
    // Destroy shared memory handle
    if (stlx_shm_destroy(shm_handle) != 0) {
        printf("STLXGFX: Failed to destroy shared memory handle for surface set\n");
        return -1;
    }
    
    __unused surface1; __unused surface2;
    return 0;
}

int stlxgfx_map_shared_surface_set(shm_handle_t shm_handle,
                                   stlxgfx_surface_t** out_surface0,
                                   stlxgfx_surface_t** out_surface1,
                                   stlxgfx_surface_t** out_surface2) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    if (!out_surface0 || !out_surface1 || !out_surface2) {
        printf("STLXGFX: Invalid output parameters for surface set mapping\n");
        return -1;
    }
    
    // Map shared memory with read/write access
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map shared memory for surface set\n");
        return -1;
    }
    
    // Get pointers to all three surfaces
    stlxgfx_surface_t* surface0 = (stlxgfx_surface_t*)shm_memory;
    
    // Calculate size of first surface to find subsequent surfaces
    size_t pixel_data_size = surface0->height * surface0->pitch;
    size_t single_surface_size = sizeof(stlxgfx_surface_t) + pixel_data_size;
    size_t aligned_surface_size = (single_surface_size + 15) & ~15;  // 16-byte align
    
    stlxgfx_surface_t* surface1 = (stlxgfx_surface_t*)((uint8_t*)shm_memory + aligned_surface_size);
    stlxgfx_surface_t* surface2 = (stlxgfx_surface_t*)((uint8_t*)shm_memory + aligned_surface_size * 2);
    
    // Validate that we got sensible surface data
    if (surface0->width == 0 || surface0->height == 0 || 
        surface1->width != surface0->width || surface1->height != surface0->height ||
        surface1->format != surface0->format ||
        surface2->width != surface0->width || surface2->height != surface0->height ||
        surface2->format != surface0->format) {
        printf("STLXGFX: Invalid surface data in shared memory for surface set\n");
        stlx_shm_unmap(shm_handle, shm_memory);
        return -1;
    }
    
    // Return surface pointers
    *out_surface0 = surface0;
    *out_surface1 = surface1;
    *out_surface2 = surface2;
    
    return 0;
}

int stlxgfx_unmap_shared_surface_set(shm_handle_t shm_handle,
                                     stlxgfx_surface_t* surface0,
                                     stlxgfx_surface_t* surface1,
                                     stlxgfx_surface_t* surface2) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory using surface0 as base address
    if (surface0) {
        if (stlx_shm_unmap(shm_handle, surface0) != 0) {
            printf("STLXGFX: Failed to unmap shared memory for surface set\n");
            return -1;
        }
    }

    __unused surface1; __unused surface2;
    return 0;
}

int stlxgfx_dm_create_window_sync_shm(stlxgfx_context_t* ctx,
                                      shm_handle_t* out_shm_handle,
                                      stlxgfx_window_sync_t** out_sync) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Window sync SHM creation only available in Display Manager mode\n");
        return -1;
    }
    
    if (!out_shm_handle || !out_sync) {
        printf("STLXGFX: Invalid output parameters for window sync SHM\n");
        return -1;
    }
    
    // Calculate memory requirements (16-byte aligned)
    size_t sync_size = sizeof(stlxgfx_window_sync_t);
    size_t aligned_size = (sync_size + 15) & ~15;  // 16-byte align
    
    // Create shared memory with unique name
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "stlxgfx_sync_%p", (void*)ctx);
    
    shm_handle_t shm_handle = stlx_shm_create(shm_name, aligned_size, SHM_READ_WRITE);
    if (shm_handle == 0) {
        printf("STLXGFX: Failed to create window sync shared memory (%zu bytes)\n", aligned_size);
        return -1;
    }
    
    // Map shared memory
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map window sync shared memory\n");
        stlx_shm_destroy(shm_handle);
        return -1;
    }
    
    // Initialize window sync structure
    stlxgfx_window_sync_t* sync = (stlxgfx_window_sync_t*)shm_memory;
    
    // Initialize all fields to safe defaults for triple buffering
    sync->front_buffer_index = 0;   // DM reads from buffer 0
    sync->back_buffer_index = 1;    // App draws to buffer 1
    sync->ready_buffer_index = 2;   // Buffer 2 is ready for swap
    sync->frame_ready = 0;          // No frame ready initially
    sync->dm_consuming = 0;         // DM not consuming initially
    sync->swap_pending = 0;         // No swap pending initially
    sync->window_visible = 0;       // Start hidden
    sync->window_focused = 0;       // Start unfocused
    sync->close_requested = 0;      // No close request initially
    sync->reserved = 0;             // Reserved field
    
    // Clear padding
    memset(sync->padding, 0, sizeof(sync->padding));
    
    // Return results
    *out_shm_handle = shm_handle;
    *out_sync = sync;
    
    return 0;
}

int stlxgfx_dm_destroy_window_sync_shm(stlxgfx_context_t* ctx,
                                       shm_handle_t shm_handle,
                                       stlxgfx_window_sync_t* sync) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Window sync SHM destruction only available in Display Manager mode\n");
        return -1;
    }
    
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory (this invalidates sync pointer)
    if (sync) {
        if (stlx_shm_unmap(shm_handle, sync) != 0) {
            printf("STLXGFX: Warning: Failed to unmap window sync shared memory\n");
            // Continue with destruction attempt
        }
    }
    
    // Destroy shared memory handle
    if (stlx_shm_destroy(shm_handle) != 0) {
        printf("STLXGFX: Failed to destroy window sync shared memory handle\n");
        return -1;
    }
    
    return 0;
}

int stlxgfx_map_window_sync_shm(shm_handle_t shm_handle,
                                stlxgfx_window_sync_t** out_sync) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    if (!out_sync) {
        printf("STLXGFX: Invalid output parameter for window sync mapping\n");
        return -1;
    }
    
    // Map shared memory with read/write access
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map window sync shared memory\n");
        return -1;
    }
    
    // Get pointer to sync structure
    stlxgfx_window_sync_t* sync = (stlxgfx_window_sync_t*)shm_memory;
    
    // Basic validation - check if structure looks reasonable
    if (sync->front_buffer_index > 2 || sync->back_buffer_index > 2 || sync->ready_buffer_index > 2) {
        printf("STLXGFX: Invalid window sync data in shared memory (bad buffer indices)\n");
        stlx_shm_unmap(shm_handle, shm_memory);
        return -1;
    }
    
    // Return sync pointer
    *out_sync = sync;
    
    return 0;
}

int stlxgfx_unmap_window_sync_shm(shm_handle_t shm_handle,
                                  stlxgfx_window_sync_t* sync) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory
    if (sync) {
        if (stlx_shm_unmap(shm_handle, sync) != 0) {
            printf("STLXGFX: Failed to unmap window sync shared memory\n");
            return -1;
        }
    }
    
    return 0;
}

int stlxgfx_draw_pixel(stlxgfx_surface_t* surface, uint32_t x, uint32_t y, uint32_t color) {
    if (!surface) {
        return -1;
    }
    
    if (x >= surface->width || y >= surface->height) {
        return -1; // Out of bounds
    }
    
    uint8_t bytes_per_pixel = stlxgfx_get_bpp_for_format(surface->format) / 8;
    uint8_t* pixel = surface->pixels + (y * surface->pitch) + (x * bytes_per_pixel);
    
    write_pixel_to_buffer(pixel, surface->format, color);
    return 0;
}

int stlxgfx_clear_surface(stlxgfx_surface_t* surface, uint32_t color) {
    if (!surface) {
        return -1;
    }
    
    uint8_t bytes_per_pixel = stlxgfx_get_bpp_for_format(surface->format) / 8;
    
    // Fast path for solid colors without alpha
    if ((color & 0xFF000000) == 0xFF000000) {
        // Check if we can use memset (all bytes same)
        uint8_t test_pixel[4] = { 0 };
        write_pixel_to_buffer(test_pixel, surface->format, color);
        
        int can_memset = 1;
        for (int i = 1; i < bytes_per_pixel; i++) {
            if (test_pixel[i] != test_pixel[0]) {
                can_memset = 0;
                break;
            }
        }
        
        if (can_memset) {
            size_t total_bytes = surface->height * surface->pitch;
            memset(surface->pixels, test_pixel[0], total_bytes);
            return 0;
        }
    }
    
    // General case: fill pixel by pixel
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t* row = surface->pixels + (y * surface->pitch);
        for (uint32_t x = 0; x < surface->width; x++) {
            write_pixel_to_buffer(row + (x * bytes_per_pixel), surface->format, color);
        }
    }
    
    return 0;
}

int stlxgfx_fill_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y, 
                      uint32_t width, uint32_t height, uint32_t color) {
    if (!surface) {
        return -1;
    }
    
    // Clip rectangle to surface bounds
    if (x >= surface->width || y >= surface->height) {
        return 0; // Completely outside
    }
    
    uint32_t end_x = x + width;
    uint32_t end_y = y + height;
    
    if (end_x > surface->width) end_x = surface->width;
    if (end_y > surface->height) end_y = surface->height;
    
    width = end_x - x;
    height = end_y - y;
    
    if (width == 0 || height == 0) {
        return 0; // Nothing to draw
    }
    
    uint8_t bytes_per_pixel = stlxgfx_get_bpp_for_format(surface->format) / 8;
    
    // Fill row by row
    for (uint32_t row = y; row < end_y; row++) {
        uint8_t* row_start = surface->pixels + (row * surface->pitch) + (x * bytes_per_pixel);
        for (uint32_t col = 0; col < width; col++) {
            write_pixel_to_buffer(row_start + (col * bytes_per_pixel), surface->format, color);
        }
    }
    
    return 0;
}

int stlxgfx_draw_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t color) {
    if (!surface || width == 0 || height == 0) {
        return -1;
    }
    
    // Draw four edges
    if (stlxgfx_fill_rect(surface, x, y, width, 1, color) != 0) return -1; // Top
    if (height > 1) {
        if (stlxgfx_fill_rect(surface, x, y + height - 1, width, 1, color) != 0) return -1; // Bottom
    }
    if (height > 2) {
        if (stlxgfx_fill_rect(surface, x, y + 1, 1, height - 2, color) != 0) return -1; // Left
        if (width > 1) {
            if (stlxgfx_fill_rect(surface, x + width - 1, y + 1, 1, height - 2, color) != 0) return -1; // Right
        }
    }
    
    return 0;
}

int stlxgfx_fill_rounded_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height, uint32_t radius, uint32_t color) {
    if (!surface || width == 0 || height == 0) {
        return -1;
    }
    
    if (radius == 0) {
        return stlxgfx_fill_rect(surface, x, y, width, height, color);
    }
    
    // Clamp radius to reasonable bounds
    uint32_t max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    
    // Fill the main rectangular areas
    if (stlxgfx_fill_rect(surface, x + radius, y, width - 2 * radius, height, color) != 0) return -1;
    if (stlxgfx_fill_rect(surface, x, y + radius, radius, height - 2 * radius, color) != 0) return -1;
    if (stlxgfx_fill_rect(surface, x + width - radius, y + radius, radius, height - 2 * radius, color) != 0) return -1;
    
    // Fill rounded corners using circle algorithm
    int r2 = radius * radius;
    for (uint32_t dy = 0; dy < radius; dy++) {
        for (uint32_t dx = 0; dx < radius; dx++) {
            int dist2 = dx * dx + dy * dy;
            if (dist2 <= r2) {
                // Fill all four corners
                stlxgfx_draw_pixel(surface, x + radius - 1 - dx, y + radius - 1 - dy, color); // Top-left
                stlxgfx_draw_pixel(surface, x + width - radius + dx, y + radius - 1 - dy, color); // Top-right
                stlxgfx_draw_pixel(surface, x + radius - 1 - dx, y + height - radius + dy, color); // Bottom-left
                stlxgfx_draw_pixel(surface, x + width - radius + dx, y + height - radius + dy, color); // Bottom-right
            }
        }
    }
    
    return 0;
}

int stlxgfx_draw_rounded_rect(stlxgfx_surface_t* surface, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height, uint32_t radius, uint32_t color) {
    if (!surface || width == 0 || height == 0) {
        return -1;
    }
    
    if (radius == 0) {
        return stlxgfx_draw_rect(surface, x, y, width, height, color);
    }
    
    // Clamp radius to reasonable bounds
    uint32_t max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    
    // Draw the straight edges
    if (stlxgfx_fill_rect(surface, x + radius, y, width - 2 * radius, 1, color) != 0) return -1; // Top
    if (stlxgfx_fill_rect(surface, x + radius, y + height - 1, width - 2 * radius, 1, color) != 0) return -1; // Bottom
    if (stlxgfx_fill_rect(surface, x, y + radius, 1, height - 2 * radius, color) != 0) return -1; // Left
    if (stlxgfx_fill_rect(surface, x + width - 1, y + radius, 1, height - 2 * radius, color) != 0) return -1; // Right
    
    // Draw rounded corners using circle outline algorithm
    int r2_outer = radius * radius;
    int r2_inner = (radius - 1) * (radius - 1);
    
    for (uint32_t dy = 0; dy < radius; dy++) {
        for (uint32_t dx = 0; dx < radius; dx++) {
            int dist2 = dx * dx + dy * dy;
            if (dist2 <= r2_outer && dist2 > r2_inner) {
                // Draw all four corners
                stlxgfx_draw_pixel(surface, x + radius - 1 - dx, y + radius - 1 - dy, color); // Top-left
                stlxgfx_draw_pixel(surface, x + width - radius + dx, y + radius - 1 - dy, color); // Top-right
                stlxgfx_draw_pixel(surface, x + radius - 1 - dx, y + height - radius + dy, color); // Bottom-left
                stlxgfx_draw_pixel(surface, x + width - radius + dx, y + height - radius + dy, color); // Bottom-right
            }
        }
    }
    
    return 0;
}

static unsigned char* get_cached_char_bitmap(stlxgfx_context_t* ctx, int codepoint, 
                                            uint32_t font_size, int* width, int* height, 
                                            int* xoff, int* yoff) {
    // Only cache printable ASCII characters (including space character 32)
    if (codepoint <= 31 || codepoint >= 32 + STLXGFX_CHAR_CACHE_SIZE) {
        // printf("CACHE: Character %d out of range\n", codepoint);
        return NULL;
    }
    
    int cache_index = codepoint - 32;
    stlxgfx_char_cache_t* cache_entry = &ctx->char_cache[cache_index];
    
    // Check if we need to invalidate cache due to font size change
    if (ctx->cached_font_size != font_size) {
        // printf("CACHE: Font size changed from %u to %u, clearing cache\n", ctx->cached_font_size, font_size);
        // Clear entire cache when font size changes
        for (int i = 0; i < STLXGFX_CHAR_CACHE_SIZE; i++) {
            if (ctx->char_cache[i].bitmap) {
                free(ctx->char_cache[i].bitmap);
                ctx->char_cache[i].bitmap = NULL;
            }
            ctx->char_cache[i].valid = 0;
        }
        ctx->cached_font_size = font_size;
    }
    
    // Check if bitmap is already cached
    if (cache_entry->valid && cache_entry->font_size == font_size) {
        // printf("CACHE: Hit for character '%c' (%d)\n", codepoint, codepoint);
        *width = cache_entry->width;
        *height = cache_entry->height;
        *xoff = cache_entry->xoff;
        *yoff = cache_entry->yoff;
        return cache_entry->bitmap;
    }
    
    // printf("CACHE: Miss for character '%c' (%d), generating bitmap\n", codepoint, codepoint);
    
    // Generate new bitmap and cache it
    float scale = stbtt_ScaleForPixelHeight(&ctx->font_info, font_size);
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&ctx->font_info, scale, scale, codepoint, 
                                                   width, height, xoff, yoff);
    
    if (bitmap && *width > 0 && *height > 0) {
        // Allocate persistent storage for the bitmap
        size_t bitmap_size = (*width) * (*height);
        cache_entry->bitmap = malloc(bitmap_size);
        if (cache_entry->bitmap) {
            memcpy(cache_entry->bitmap, bitmap, bitmap_size);
            cache_entry->width = *width;
            cache_entry->height = *height;
            cache_entry->xoff = *xoff;
            cache_entry->yoff = *yoff;
            cache_entry->font_size = font_size;
            cache_entry->valid = 1;
            // printf("CACHE: Cached character '%c' (%d), size %dx%d\n", codepoint, codepoint, *width, *height);
        }
        
        // Free the STB allocated bitmap
        stbtt_FreeBitmap(bitmap, NULL);
        
        // Return our cached copy
        return cache_entry->bitmap;
    }
    
    return NULL;
}

int stlxgfx_render_text(stlxgfx_context_t* ctx, stlxgfx_surface_t* surface,
                        const char* text, uint32_t x, uint32_t y, 
                        uint32_t font_size, uint32_t color) {
    if (!ctx || !ctx->initialized || !ctx->font_loaded || !surface || !text) {
        return -1;
    }
    
    float scale = stbtt_ScaleForPixelHeight(&ctx->font_info, font_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&ctx->font_info, &ascent, &descent, &line_gap);
    
    int baseline_y = y + (int)(ascent * scale);
    int current_x = x;
    
    for (int i = 0; text[i]; i++) {
        int codepoint = text[i];
        
        // Get character metrics
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&ctx->font_info, codepoint, &advance, &lsb);
        
        // Try to get cached bitmap first
        int char_width, char_height, xoff, yoff;
        unsigned char* bitmap = get_cached_char_bitmap(ctx, codepoint, font_size, 
                                                       &char_width, &char_height, &xoff, &yoff);
        
        // If not cached, fall back to direct generation (for non-ASCII chars)
        int should_free_bitmap = 0;
        if (!bitmap) {
            bitmap = stbtt_GetCodepointBitmap(&ctx->font_info, scale, scale, codepoint, 
                                           &char_width, &char_height, &xoff, &yoff);
            should_free_bitmap = 1;
        }
        
        if (bitmap && char_width > 0 && char_height > 0) {
            // Draw character to surface
            int char_x = current_x + (int)(lsb * scale) + xoff;
            int char_y = baseline_y + yoff;
            
            for (int py = 0; py < char_height; py++) {
                for (int px = 0; px < char_width; px++) {
                    int surface_x = char_x + px;
                    int surface_y = char_y + py;
                    
                    if (surface_x >= 0 && surface_x < (int)surface->width && 
                        surface_y >= 0 && surface_y < (int)surface->height) {
                        
                        unsigned char alpha = bitmap[py * char_width + px];
                        if (alpha > 0) {
                            // Create color with alpha
                            uint32_t text_color = (color & 0x00FFFFFF) | ((uint32_t)alpha << 24);
                            
                            uint8_t bytes_per_pixel = stlxgfx_get_bpp_for_format(surface->format) / 8;
                            uint8_t* pixel = surface->pixels + (surface_y * surface->pitch) + (surface_x * bytes_per_pixel);
                            
                            alpha_blend_pixel(pixel, surface->format, text_color);
                        }
                    }
                }
            }
            
            // Only free if this was a direct STB allocation (not cached)
            if (should_free_bitmap) {
            stbtt_FreeBitmap(bitmap, NULL);
            }
        }
        
        // Advance to next character
        current_x += (int)(advance * scale);
    }
    
    return 0;
}

int stlxgfx_blit_surface(stlxgfx_surface_t* src, uint32_t src_x, uint32_t src_y,
                         stlxgfx_surface_t* dst, uint32_t dst_x, uint32_t dst_y,
                         uint32_t width, uint32_t height) {
    if (!src || !dst) {
        return -1;
    }
    
    // Clip to source bounds
    if (src_x >= src->width || src_y >= src->height) {
        return 0; // Completely outside source
    }
    
    uint32_t src_end_x = src_x + width;
    uint32_t src_end_y = src_y + height;
    
    if (src_end_x > src->width) {
        width = src->width - src_x;
        src_end_x = src->width;
    }
    if (src_end_y > src->height) {
        height = src->height - src_y;
        src_end_y = src->height;
    }
    
    // Clip to destination bounds
    if (dst_x >= dst->width || dst_y >= dst->height) {
        return 0; // Completely outside destination
    }
    
    uint32_t dst_end_x = dst_x + width;
    uint32_t dst_end_y = dst_y + height;
    
    if (dst_end_x > dst->width) {
        width = dst->width - dst_x;
        dst_end_x = dst->width;
    }
    if (dst_end_y > dst->height) {
        height = dst->height - dst_y;
        dst_end_y = dst->height;
    }
    
    if (width == 0 || height == 0) {
        return 0; // Nothing to copy
    }
    
    uint8_t src_bpp = stlxgfx_get_bpp_for_format(src->format) / 8;
    uint8_t dst_bpp = stlxgfx_get_bpp_for_format(dst->format) / 8;
    
    // Fast path: same format
    if (src->format == dst->format) {
        for (uint32_t y = 0; y < height; y++) {
            uint8_t* src_row = src->pixels + ((src_y + y) * src->pitch) + (src_x * src_bpp);
            uint8_t* dst_row = dst->pixels + ((dst_y + y) * dst->pitch) + (dst_x * dst_bpp);
            memcpy(dst_row, src_row, width * src_bpp);
        }
    } else {
        // Format conversion required
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t* src_pixel = src->pixels + ((src_y + y) * src->pitch) + ((src_x + x) * src_bpp);
                uint8_t* dst_pixel = dst->pixels + ((dst_y + y) * dst->pitch) + ((dst_x + x) * dst_bpp);
                
                uint32_t color = read_pixel_from_buffer(src_pixel, src->format);
                write_pixel_to_buffer(dst_pixel, dst->format, color);
            }
        }
    }
    
    return 0;
}

int stlxgfx_blit_surface_to_buffer(stlxgfx_surface_t* surface, uint8_t* buffer, uint32_t buffer_pitch) {
    if (!surface || !buffer) {
        return -1;
    }
    
    // Copy row by row
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t* src_row = surface->pixels + (y * surface->pitch);
        uint8_t* dst_row = buffer + (y * buffer_pitch);
        memcpy(dst_row, src_row, surface->pitch);
    }
    
    return 0;
} 