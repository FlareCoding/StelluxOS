#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "stlxdm.h"
#include <stlxgfx/stlxgfx.h>

int stlxdm_get_framebuffer_info(struct gfx_framebuffer_info* fb_info) {
    if (!fb_info) {
        printf("ERROR: NULL framebuffer info pointer\n");
        return -1;
    }
    
    long result = syscall2(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_GET_INFO, (uint64_t)fb_info);
    if (result != 0) {
        printf("ERROR: Failed to get framebuffer info: %ld\n", result);
        return -1;
    }
    
    printf("STLXDM] Framebuffer info: %ux%u, %u BPP, pitch=%u, size=%u\n", 
           fb_info->width, fb_info->height, fb_info->bpp, fb_info->pitch, fb_info->size);
    
    return 0;
}

uint8_t* stlxdm_map_framebuffer(void) {
    long map_result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_MAP_FRAMEBUFFER);
    if (map_result <= 0) {
        printf("ERROR: Failed to map framebuffer: %ld\n", map_result);
        return NULL;
    }
    
    printf("[STLXDM] Framebuffer mapped at address: 0x%lx\n", map_result);
    return (uint8_t*)map_result;
}

int stlxdm_unmap_framebuffer(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_UNMAP_FRAMEBUFFER);
    if (result != 0) {
        printf("ERROR: Failed to unmap framebuffer: %ld\n", result);
        return -1;
    }
    
    printf("Framebuffer unmapped successfully\n");
    return 0;
}

// ====================== //
//    Main Entry Point    //
// ====================== //

int main() {
    printf("StelluxOS Display Manager (STLXDM) - Initializing...\n");

    // Initialize framebuffer first
    struct gfx_framebuffer_info fb_info;
    if (stlxdm_get_framebuffer_info(&fb_info) != 0) {
        return 1;
    }
    
    uint8_t* framebuffer = stlxdm_map_framebuffer();
    if (!framebuffer) {
        return 1;
    }
    
    // Initialize graphics library
    stlxgfx_context_t* gfx_ctx = stlxgfx_init(STLXGFX_MODE_DISPLAY_MANAGER);
    if (!gfx_ctx) {
        printf("ERROR: Failed to initialize graphics library\n");
        stlxdm_unmap_framebuffer();
        return 1;
    }

    const char* font_path = "/initrd/res/fonts/UbuntuMono-Regular.ttf";
    if (stlxgfx_dm_load_font(gfx_ctx, font_path) != 0) {
        printf("ERROR: Failed to load stlxgfx font\n");
        stlxgfx_cleanup(gfx_ctx);
        stlxdm_unmap_framebuffer();
        return 1;
    }
    
    stlxgfx_font_metrics_t metrics;
    if (stlxgfx_dm_get_font_metrics(gfx_ctx, &metrics) == 0) {
        printf("Font metrics retrieved successfully!\n");
    }
    
    stlxgfx_text_size_t text_size;
    if (stlxgfx_get_text_size(gfx_ctx, "Hello from StelluxOS!", 32, &text_size) == 0) {
        printf("Text size calculated successfully!\n");
    }
    
    // Create compositor surface matching framebuffer format
    stlxgfx_surface_t* compositor_surface = stlxgfx_dm_create_surface(gfx_ctx, fb_info.width, fb_info.height, stlxgfx_detect_gop_format(fb_info.bpp));
    if (compositor_surface) {
        printf("Surface created successfully! %ux%u, pitch=%u\n", 
               compositor_surface->width, compositor_surface->height, compositor_surface->pitch);
        
        uint8_t bpp = stlxgfx_get_bpp_for_format(compositor_surface->format);
        printf("Surface format BPP: %u\n", bpp);
        
        // Test drawing primitives
        printf("[STLXDM] Testing drawing primitives...\n");
        
        // Clear surface with dark gray background
        if (stlxgfx_clear_surface(compositor_surface, 0xFF202020) == 0) {
            printf("   Clear surface test passed\n");
        }
        
        // Test colored rectangles in corners
        if (stlxgfx_fill_rect(compositor_surface, 50, 50, 100, 100, 0xFFFF0000) == 0) { // Red top-left
            printf("   Fill rect (red) test passed\n");
        }
        
        if (stlxgfx_fill_rect(compositor_surface, fb_info.width - 150, 50, 100, 100, 0xFF00FF00) == 0) { // Green top-right
            printf("   Fill rect (green) test passed\n");
        }
        
        if (stlxgfx_fill_rect(compositor_surface, 50, fb_info.height - 150, 100, 100, 0xFF0000FF) == 0) { // Blue bottom-left
            printf("   Fill rect (blue) test passed\n");
        }
        
        if (stlxgfx_fill_rect(compositor_surface, fb_info.width - 150, fb_info.height - 150, 100, 100, 0xFFFFFFFF) == 0) { // White bottom-right
            printf("   Fill rect (white) test passed\n");
        }
        
        // Test rectangle outlines
        if (stlxgfx_draw_rect(compositor_surface, 200, 100, 200, 150, 0xFFFFFF00) == 0) { // Yellow outline
            printf("   Draw rect outline test passed\n");
        }
        
        // Test rounded rectangles
        uint32_t center_x = fb_info.width / 2;
        uint32_t center_y = fb_info.height / 2;
        
        if (stlxgfx_fill_rounded_rect(compositor_surface, center_x - 100, center_y - 50, 200, 100, 20, 0xFF800080) == 0) { // Purple rounded rect
            printf("   Fill rounded rect test passed\n");
        }
        
        if (stlxgfx_draw_rounded_rect(compositor_surface, center_x - 120, center_y - 70, 240, 140, 25, 0xFF00FFFF) == 0) { // Cyan rounded outline
            printf("   Draw rounded rect outline test passed\n");
        }
        
        // Test individual pixels
        for (int i = 0; i < 20; i++) {
            uint32_t color = 0xFF000000 | (i * 12) | ((i * 8) << 8) | ((i * 15) << 16);
            stlxgfx_draw_pixel(compositor_surface, 300 + i * 2, 200, color);
        }
        printf("   Draw pixel test passed\n");
        
        // Test text rendering
        if (stlxgfx_render_text(gfx_ctx, compositor_surface, "StelluxOS Display Manager", 
                               center_x - 150, center_y + 80, 24, 0xFFFFFFFF) == 0) {
            printf("   Text rendering test passed\n");
        }
        
        if (stlxgfx_render_text(gfx_ctx, compositor_surface, "Graphics Library v0.1", 
                               center_x - 100, center_y + 110, 18, 0xFF00FFFF) == 0) {
            printf("   Text rendering (smaller) test passed\n");
        }
        
        // Test gradient effect
        printf("  Creating gradient effect...\n");
        for (uint32_t y = center_y - 20; y < center_y + 20; y++) {
            for (uint32_t x = center_x - 80; x < center_x + 80; x++) {
                uint32_t red = (255 * (x - (center_x - 80))) / 160;
                uint32_t blue = (255 * (y - (center_y - 20))) / 40;
                uint32_t color = 0xFF000000 | (red << 16) | blue;
                stlxgfx_draw_pixel(compositor_surface, x, y, color);
            }
        }
        printf("   Gradient effect test passed\n");
        
        // Copy compositor surface to framebuffer
        printf("[STLXDM] Copying surface to framebuffer...\n");
        if (stlxgfx_blit_surface_to_buffer(compositor_surface, framebuffer, fb_info.pitch) == 0) {
            printf("   Surface to framebuffer blit successful!\n");
            printf("[STLXDM] Display should now show graphics test pattern\n");
        } else {
            printf("  ✗ Failed to blit surface to framebuffer\n");
        }
        
        stlxgfx_dm_destroy_surface(gfx_ctx, compositor_surface);
    } else {
        printf("Failed to create compositor surface!\n");
    }
    
    
    

    // Clean up
    stlxgfx_cleanup(gfx_ctx);
    stlxdm_unmap_framebuffer();
    return 0;
}
