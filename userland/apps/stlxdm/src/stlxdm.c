#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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

int stlxdm_begin_frame(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_DISABLE_PREEMPT);
    if (result != 0) {
        printf("ERROR: Failed to disable preemption: %ld\n", result);
        return -1;
    }
    
    return 0;
}

int stlxdm_end_frame(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_ENABLE_PREEMPT);
    if (result != 0) {
        printf("ERROR: Failed to enable preemption: %ld\n", result);
        return -1;
    }
    
    return 0;
}

// ====================== //
//    Main Entry Point    //
// ====================== //

int main() {
    printf("StelluxOS Display Manager (STLXDM) - Initializing...\n");
    
    // === UNIX DOMAIN SOCKET SERVER TEST ===
    printf("[STLXDM] Testing Unix Domain Socket server...\n");
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("[STLXDM] ERROR: socket() failed: %d\n", errno);
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/stlxdm.socket");
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[STLXDM] ERROR: bind() failed: %d\n", errno);
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, 1) < 0) {
        printf("[STLXDM] ERROR: listen() failed: %d\n", errno);
        close(server_fd);
        return 1;
    }
    
    printf("[STLXDM] Server listening on /tmp/stlxdm.socket\n");
    
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        printf("[STLXDM] ERROR: accept() failed: %d\n", errno);
        close(server_fd);
        return 1;
    }

    printf("[STLXDM] Client connected with fd %i\n", client_fd);
    
    char buffer[256];
    ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
    printf("[STLXDM] Read returned %ld bytes\n", bytes);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        printf("[STLXDM] Received: '%s'\n", buffer);
    } else if (bytes == 0) {
        printf("[STLXDM] Connection closed by peer (EOF)\n");
    } else {
        printf("[STLXDM] Read error: %ld (errno: %d)\n", bytes, errno);
    }
    
    close(client_fd);
    close(server_fd);

    printf("[STLXDM] Unix Domain Socket test completed!\n");

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
        
        // Create stylish display manager interface
        
        uint32_t center_x = fb_info.width / 2;
        uint32_t banner_y = fb_info.height / 3;
        
        // Animation loop for flashing text
        uint64_t frame = 0;
        while (1) {
            ++frame;

            // Clear surface with dark background
            stlxgfx_clear_surface(compositor_surface, 0xFF0A0A0A);
            
            // Create modern gradient banner
            uint32_t banner_width = 600;
            uint32_t banner_height = 80;
            uint32_t banner_x = center_x - banner_width / 2;
            
            for (uint32_t y = banner_y; y < banner_y + banner_height; y++) {
                for (uint32_t x = banner_x; x < banner_x + banner_width; x++) {
                    // Create a modern blue-to-purple gradient
                    float progress_x = (float)(x - banner_x) / banner_width;
                    float progress_y = (float)(y - banner_y) / banner_height;
                    
                    // Blue to purple gradient with some vertical variation
                    uint32_t red = (uint32_t)(80 + progress_x * 120 + progress_y * 30);
                    uint32_t green = (uint32_t)(60 + progress_y * 40);
                    uint32_t blue = (uint32_t)(200 - progress_x * 80 + progress_y * 20);
                    
                    // Clamp values
                    if (red > 255) red = 255;
                    if (green > 255) green = 255;
                    if (blue > 255) blue = 255;
                    
                    uint32_t color = 0xFF000000 | (red << 16) | (green << 8) | blue;
                    stlxgfx_draw_pixel(compositor_surface, x, y, color);
                }
            }
            
            // Draw cyan rounded rectangle border around banner
            stlxgfx_draw_rounded_rect(compositor_surface, banner_x - 10, banner_y - 10,
                                    banner_width + 20, banner_height + 20, 15, 0xFF00FFFF);
            
            // Add title text underneath the banner
            stlxgfx_render_text(gfx_ctx, compositor_surface, "StelluxOS Display Manager", 
                               center_x - 180, banner_y + banner_height + 30, 28, 0xFFFFFFFF);
            
            // Fading "Press enter to unlock..." text
            uint32_t unlock_y = banner_y + banner_height + 130;
            
            // Create smooth breathing effect with constant speed
            float breathing_speed = 0.24f;
            float fade_factor = (sinf(frame * breathing_speed) + 1.0f) / 2.0f; // 0.0 to 1.0
            
            // Interpolate between transparent gray (0x00808080) and white (0xFFFFFFFF)
            // At fade_factor = 0.0: fully transparent gray (0x00808080)
            // At fade_factor = 1.0: fully opaque white (0xFFFFFFFF)
            uint32_t alpha = (uint32_t)(0x00 + (0xFF - 0x00) * fade_factor);
            uint32_t rgb = (uint32_t)(0x80 + (0xFF - 0x80) * fade_factor);
            
            uint32_t text_color = (alpha << 24) | (rgb << 16) | (rgb << 8) | rgb;
            
            // Calculate text width for perfect centering
            const char* unlock_text = "Press enter to unlock...";
            stlxgfx_text_size_t unlock_text_size;
            if (stlxgfx_get_text_size(gfx_ctx, unlock_text, 32, &unlock_text_size) == 0) {
                uint32_t text_x = center_x - (unlock_text_size.width / 2);
                stlxgfx_render_text(gfx_ctx, compositor_surface, unlock_text, 
                                   text_x, unlock_y, 32, text_color);
            } else {
                // Fallback to approximate centering if text size calculation fails
                stlxgfx_render_text(gfx_ctx, compositor_surface, unlock_text, 
                                   center_x - 160, unlock_y, 32, text_color);
            }
            
            // Copy compositor surface to framebuffer
            stlxdm_begin_frame();
            stlxgfx_blit_surface_to_buffer(compositor_surface, framebuffer, fb_info.pitch);
            stlxdm_end_frame();
            
            // Small delay for animation timing
            struct timespec ts = { 0, 1 * 1000 * 1000 }; // 1ms
            nanosleep(&ts, NULL);
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
