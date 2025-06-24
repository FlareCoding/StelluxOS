#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define _POSIX_C_SOURCE 199309L
#include <time.h>

#include <stlibc/stlibc.h>
#include <stlxgfx/stlxgfx.h>

int main() {
    // Wait for display manager to be ready
    struct timespec ts = { 5, 0 }; // 5 seconds
    nanosleep(&ts, NULL);
    
    // Initialize graphics library in application mode
    printf("[SHELL] Initializing graphics library in application mode...\n");
    stlxgfx_context_t* ctx = stlxgfx_init(STLXGFX_MODE_APPLICATION);
    if (!ctx) {
        printf("[SHELL] ERROR: Failed to initialize graphics library\n");
        return 1;
    }
    
    printf("[SHELL] Graphics library initialized successfully\n");

    struct timespec ts2 = { 0, 2 * 1000 * 1000 }; // 2 ms
    nanosleep(&ts2, NULL);
    
    // Create a test window
    printf("[SHELL] Creating window (800x600)...\n");
    stlxgfx_window_t* window = stlxgfx_create_window(ctx, 460, 340);
    if (!window) {
        printf("[SHELL] ERROR: Failed to create window\n");
        stlxgfx_cleanup(ctx);
        return 1;
    }
    
    printf("[SHELL] Window created successfully!\n");
    
    // Test actual drawing functions on the surface
    printf("[SHELL] Drawing to surface...\n");
    
    // Keep the window "alive" for demonstration
    while (1) {
        // Get the active drawing surface for this frame
        stlxgfx_surface_t* surface = stlxgfx_get_active_surface(window);
        if (!surface) {
            printf("[SHELL] ERROR: Failed to get active surface for frame %d\n", 11-i);
            break;
        }
        
        printf("[SHELL] Drawing to surface %ux%u (frame %d)...\n", surface->width, surface->height, 11-i);
        
        // Clear surface with dark gray background
        stlxgfx_clear_surface(surface, 0xFF202020);
        
        // Draw some test content
        stlxgfx_draw_pixel(surface, 100, 100, 0xFFFFFFFF);  // White pixel
        stlxgfx_fill_rect(surface, 50, 50, 200, 100, 0xFF0066CC);  // Blue rectangle
        stlxgfx_fill_rect(surface, 300, 200, 150, 80, 0xFF00AA44);  // Green rectangle
        
        // Add some more visual elements for testing
        stlxgfx_fill_rect(surface, 20, 20, 10, 10, 0xFFFF0000);    // Red square (top-left)
        stlxgfx_fill_rect(surface, surface->width-30, 20, 10, 10, 0xFFFF0000);  // Red square (top-right)
        stlxgfx_fill_rect(surface, 20, surface->height-30, 10, 10, 0xFFFF0000); // Red square (bottom-left)
        stlxgfx_fill_rect(surface, surface->width-30, surface->height-30, 10, 10, 0xFFFF0000); // Red square (bottom-right)
        
        printf("[SHELL] Drawing completed, swapping buffers...\n");
        
        // Swap buffers to present the frame
        int swap_result = stlxgfx_swap_buffers(window);
        if (swap_result != 0) {
            printf("[SHELL] WARNING: Buffer swap failed with code %d\n", swap_result);
        }
        nanosleep(&ts2, NULL);
    }
    
    // Clean up
    printf("[SHELL] Cleaning up window and graphics context...\n");
    stlxgfx_destroy_window(ctx, window);
    stlxgfx_cleanup(ctx);
    
    printf("[SHELL] Example window application completed successfully!\n");
    return 0;
}
