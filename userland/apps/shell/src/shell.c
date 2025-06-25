#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    
    // Create a test window
    printf("[SHELL] Creating window (460x340) at position (200, 150) with title...\n");
    stlxgfx_window_t* window = stlxgfx_create_window(ctx, 460, 340, 200, 150, "Demo App");
    if (!window) {
        printf("[SHELL] ERROR: Failed to create window\n");
        stlxgfx_cleanup(ctx);
        return 1;
    }
    
    printf("[SHELL] Window created successfully!\n");
    
    // Animation variables for bouncing cube
    int cube_x = 50;        // Cube X position
    int cube_y = 150;       // Cube Y position (fixed)
    int cube_size = 40;     // Cube size
    int velocity_x = 3;     // Horizontal velocity
    int window_width = 460;
    int window_height = 340;
    
    // Simple frame counter
    uint32_t frame_count = 0;
    
    printf("[SHELL] Starting bouncing cube animation with on-screen info...\n");
    
    // Keep the window "alive" with animation
    while (1) {
        // Get the active drawing surface for this frame
        stlxgfx_surface_t* surface = stlxgfx_get_active_surface(window);
        if (!surface) {
            printf("[SHELL] ERROR: Failed to get active surface\n");
            break;
        }
        
        // Clear surface with dark brown background
        stlxgfx_clear_surface(surface, 0xFF362616);
        
        // Update cube position
        cube_x += velocity_x;
        
        // Bounce off walls
        if (cube_x <= 0) {
            cube_x = 0;
            velocity_x = 3; // Move right
        } else if (cube_x + cube_size >= window_width) {
            cube_x = window_width - cube_size;
            velocity_x = -3; // Move left
        }
        
        // Draw the bouncing cube (light gray)
        stlxgfx_fill_rect(surface, cube_x, cube_y, cube_size, cube_size, 0xFFA5A5A5);
        
        // Add some static elements for reference
        stlxgfx_fill_rect(surface, 20, 20, 10, 10, 0xFFFF0000);  // Red corner marker (top-left)
        stlxgfx_fill_rect(surface, window_width - 30, 20, 10, 10, 0xFFFF0000); // Red corner marker (top-right)
        stlxgfx_fill_rect(surface, 20, window_height - 30, 10, 10, 0xFFFF0000); // Red corner marker (bottom-left)
        stlxgfx_fill_rect(surface, window_width - 30, window_height - 30, 10, 10, 0xFFFF0000); // Red corner marker (bottom-right)
        
        // Add a center reference line
        stlxgfx_fill_rect(surface, window_width / 2 - 1, 0, 2, window_height, 0xFF444444);
        
        // Increment frame counter
        frame_count++;
        
        // Render animation info in top-left corner (below the red marker)
        char pos_text[32];
        snprintf(pos_text, sizeof(pos_text), "Position: %d", cube_x);
        stlxgfx_render_text(ctx, surface, pos_text, 20, 50, 16, 0xFFFFFFFF); // White text, 16px font
        
        // Render velocity info
        char vel_text[32];
        snprintf(vel_text, sizeof(vel_text), "Velocity: %d", velocity_x);
        stlxgfx_render_text(ctx, surface, vel_text, 20, 75, 14, 0xFFCCCCCC); // Light gray text, 14px font
        
        // Render frame count
        char frame_text[32];
        snprintf(frame_text, sizeof(frame_text), "Frame: %u", frame_count);
        stlxgfx_render_text(ctx, surface, frame_text, 20, 100, 14, 0xFFCCCCCC); // Light gray text
        
        // Render status in top-right corner
        const char* direction = (velocity_x > 0) ? "Right" : "Left";
        char status_text[32];
        snprintf(status_text, sizeof(status_text), "Moving: %s", direction);
        stlxgfx_render_text(ctx, surface, status_text, window_width - 120, 50, 14, 0xFFAAFFAA); // Light green text
        
        // Swap buffers to present the frame
        int swap_result = stlxgfx_swap_buffers(window);
        if (swap_result == -3) {
            // In triple buffering, we can continue drawing even if swap is pending
            // This demonstrates the non-blocking nature of triple buffering
        } else if (swap_result != 0) {
            printf("[SHELL] ERROR: Buffer swap failed with code %d\n", swap_result);
            break;
        }
        
        // Small delay to control animation speed (about 60 FPS)
        struct timespec frame_delay = { 0, 16 * 1000 * 1000 }; // 16ms = ~60 FPS
        nanosleep(&frame_delay, NULL);
    }
    
    // Clean up
    printf("[SHELL] Cleaning up window and graphics context...\n");
    stlxgfx_destroy_window(ctx, window);
    stlxgfx_cleanup(ctx);
    
    printf("[SHELL] Bouncing cube animation completed successfully!\n");
    return 0;
}
