#include "stlxdm_compositor.h"
#include "stlxdm_server.h"
#include <stdio.h>
#include <string.h>
#include <stlxgfx/internal/stlxgfx_dm.h>
#include <stlxgfx/window.h>

int stlxdm_compositor_init(stlxdm_compositor_t* compositor, stlxgfx_context_t* gfx_ctx) {
    if (!compositor || !gfx_ctx) {
        printf("[STLXDM_COMPOSITOR] ERROR: Invalid parameters for compositor init\n");
        return -1;
    }
    
    // Initialize compositor context
    memset(compositor, 0, sizeof(stlxdm_compositor_t));
    compositor->gfx_ctx = gfx_ctx;
    
    // Initialize framebuffer
    if (stlxdm_get_framebuffer_info(&compositor->fb_info) != 0) {
        printf("[STLXDM_COMPOSITOR] ERROR: Failed to get framebuffer info\n");
        return -1;
    }

    // Detect the native framebuffer format
    compositor->gop_format = stlxgfx_detect_gop_format(compositor->fb_info.bpp);
    
    // Map the physical framebuffer
    compositor->framebuffer = stlxdm_map_framebuffer();
    if (!compositor->framebuffer) {
        printf("[STLXDM_COMPOSITOR] ERROR: Failed to map framebuffer\n");
        return -1;
    }
    
    // Create compositor surface matching framebuffer format
    compositor->compositor_surface = stlxgfx_dm_create_surface(
        gfx_ctx, 
        compositor->fb_info.width, 
        compositor->fb_info.height, 
        stlxgfx_detect_gop_format(compositor->fb_info.bpp)
    );
    
    if (!compositor->compositor_surface) {
        printf("[STLXDM_COMPOSITOR] ERROR: Failed to create compositor surface\n");
        stlxdm_unmap_framebuffer();
        return -1;
    }
    
    compositor->initialized = 1;
    
    return 0;
}

void stlxdm_compositor_cleanup(stlxdm_compositor_t* compositor) {
    if (!compositor || !compositor->initialized) {
        return;
    }
    
    // Destroy the compositor surface
    if (compositor->compositor_surface && compositor->gfx_ctx) {
        stlxgfx_dm_destroy_surface(compositor->gfx_ctx, compositor->compositor_surface);
        compositor->compositor_surface = NULL;
    }
    
    // Unmap framebuffer
    if (compositor->framebuffer) {
        stlxdm_unmap_framebuffer();
        compositor->framebuffer = NULL;
    }
    
    compositor->initialized = 0;
}

int stlxdm_compositor_compose(stlxdm_compositor_t* compositor, void* server, int32_t cursor_x, int32_t cursor_y, uint32_t focused_window_id) {
    if (!compositor || !compositor->initialized) {
        return -1;
    }
    
    if (!server) {
        return -1;
    }
    
    // Clear the compositor surface with a dark background
    stlxgfx_clear_surface(compositor->compositor_surface, 0x100f10);
    
    // Fixed window position for now
    const int window_x = 140;
    const int window_y = 140 + 32 + 1; // Add title bar height + border width
    
    // Iterate through all connected clients and composite their windows
    for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
        stlxdm_server_t* server_ptr = (stlxdm_server_t*)server;
        stlxdm_client_info_t* client = &server_ptr->clients[i];
        
        // Skip disconnected clients or clients without windows
        if (client->state != STLXDM_CLIENT_CONNECTED || !client->window) {
            continue;
        }
        
        stlxgfx_window_t* window = client->window;
        
        // Check if this window has a new frame ready for synchronization
        int sync_result = stlxgfx_dm_sync_window(window);
        
        // Get the current DM surface (the one DM should read from)
        stlxgfx_surface_t* dm_surface = stlxgfx_get_dm_surface(window);
        if (!dm_surface) {
            if (sync_result > 0) {
                stlxgfx_dm_finish_sync_window(window); // Clean up sync state if we started it
            }
            continue;
        }
        
        // Check if window + decorations fit on screen
        const int decoration_width = 2;  // 2 * border_width (1 * 2)
        const int decoration_height = 34; // title_bar_height (32) + 2 * border_width (1 * 2)
        if (window_x - 1 + window->width + decoration_width > compositor->compositor_surface->width ||
            window_y - 33 + window->height + decoration_height > compositor->compositor_surface->height) {
            if (sync_result > 0) {
                stlxgfx_dm_finish_sync_window(window); // Clean up sync state if we started it
            }
            continue;
        }
        
        // Blit the window surface to the compositor surface at fixed position
        // We always composite the DM surface, regardless of whether there's a new frame
        int blit_result = stlxgfx_blit_surface(
            dm_surface, 0, 0,                       // Source: DM surface
            compositor->compositor_surface, window_x, window_y,  // Dest: fixed position
            window->width, window->height           // Size: full window
        );
        
        // Finish synchronization only if we started it (sync_result > 0)
        if (sync_result > 0) {
            stlxgfx_dm_finish_sync_window(window);
        }
        
        if (blit_result != 0) {
            printf("[STLXDM_COMPOSITOR] Failed to blit window %u\n", window->window_id);
            continue;
        }
        
        // Draw window decorations around the blitted window content
        int is_focused = (window->window_id == focused_window_id) ? 1 : 0;
        stlxdm_compositor_draw_window_decorations(
            compositor, 
            window_x, window_y, 
            window->width, window->height,
            window->window_id, 
            is_focused
        );
    }
    
    // Render cursor if valid position provided
    if (cursor_x >= 0 && cursor_y >= 0) {
        stlxdm_compositor_draw_cursor(compositor, cursor_x, cursor_y);
    }
    
    return 0;
}

int stlxdm_compositor_present(stlxdm_compositor_t* compositor) {
    if (!compositor || !compositor->initialized || !compositor->compositor_surface || !compositor->framebuffer) {
        return -1;
    }

    int ret;
    
    // Copy compositor surface to framebuffer
    stlxdm_begin_frame();
    ret = stlxgfx_blit_surface_to_buffer(compositor->compositor_surface, 
                                         compositor->framebuffer, 
                                         compositor->fb_info.pitch);
    stlxdm_end_frame();

    return ret;
}

const struct gfx_framebuffer_info* stlxdm_compositor_get_fb_info(const stlxdm_compositor_t* compositor) {
    if (!compositor || !compositor->initialized) {
        return NULL;
    }
    return &compositor->fb_info;
}

stlxgfx_surface_t* stlxdm_compositor_get_surface(const stlxdm_compositor_t* compositor) {
    if (!compositor || !compositor->initialized) {
        return NULL;
    }
    return compositor->compositor_surface;
}

void stlxdm_compositor_draw_cursor(stlxdm_compositor_t* compositor, int32_t x, int32_t y) {
    if (!compositor || !compositor->initialized || !compositor->compositor_surface) {
        return;
    }
    
    stlxgfx_surface_t* surface = compositor->compositor_surface;
    
    // Modern cursor design - sleek arrow with shadow
    static const char* cursor_shape[16] = {
        "X                 ",
        "XX                ",
        "X.X               ",
        "X..X              ",
        "X...X             ",
        "X....X            ",
        "X.....X           ",
        "X......X          ",
        "X.......X         ",
        "X........X        ",
        "X.........X       ",
        "X..........X      ",
        "X......XXXXXXX    ",
        "X...XX            ",
        "X..X              ",
        "XX                "
    };
    
    // Modern colors with subtle shadow
    uint32_t outline = 0xFF000000;      // Black outline
    uint32_t fill = 0xFFFFFFFF;         // White fill
    uint32_t shadow = 0x80000000;       // Semi-transparent black shadow
    
    const int cursor_width = 18;
    const int cursor_height = 16;
    
    // Check bounds
    if (x < 0 || y < 0 || 
        x + cursor_width >= (int32_t)surface->width || 
        y + cursor_height >= (int32_t)surface->height) {
        return;
    }
    
    // First pass: Draw shadow (offset by 1 pixel down and right)
    for (int row = 0; row < cursor_height; row++) {
        for (int col = 0; col < cursor_width; col++) {
            char pixel = cursor_shape[row][col];
            if (pixel == 'X' || pixel == '.') {
                int shadow_x = x + col + 1;
                int shadow_y = y + row + 1;
                if (shadow_x < (int32_t)surface->width && shadow_y < (int32_t)surface->height) {
                    stlxgfx_draw_pixel(surface, shadow_x, shadow_y, shadow);
                }
            }
        }
    }
    
    // Second pass: Draw main cursor
    for (int row = 0; row < cursor_height; row++) {
        for (int col = 0; col < cursor_width; col++) {
            char pixel = cursor_shape[row][col];
            if (pixel == 'X') {
                // Black outline
                stlxgfx_draw_pixel(surface, x + col, y + row, outline);
            } else if (pixel == '.') {
                // White fill
                stlxgfx_draw_pixel(surface, x + col, y + row, fill);
            }
        }
    }
}

void stlxdm_compositor_draw_window_decorations(stlxdm_compositor_t* compositor, 
                                               int32_t window_x, int32_t window_y,
                                               uint32_t window_width, uint32_t window_height,
                                               uint32_t window_id, int is_focused) {
    if (!compositor || !compositor->initialized || !compositor->compositor_surface) {
        return;
    }
    
    stlxgfx_surface_t* surface = compositor->compositor_surface;
    
    // Window decoration constants
    const int title_bar_height = 32;   // Slightly taller for modern look
    const int border_width = 1;        // Thinner borders for cleaner appearance
    const int close_button_size = 14;  // Slightly smaller button
    const int close_button_margin = 9; // (32 - 14) / 2 = 9 for centering
    
    // Modern dark color scheme
    uint32_t border_color = is_focused ? 0xFF2D2D30 : 0xFF1E1E1E;  // Dark gray borders
    uint32_t title_bar_color = is_focused ? 0xFF3C3C3C : 0xFF2B2B2B;  // Dark title bars
    uint32_t title_text_color = is_focused ? 0xFFFFFFFF : 0xFFBBBBBB;  // White/light gray text
    uint32_t close_button_bg = 0xFF404040;   // Dark gray close button background
    uint32_t close_button_hover = 0xFFE81123;  // Red on hover (we'll use this as default for now)
    uint32_t close_button_x_color = 0xFFFFFFFF;  // White X
    
    // Calculate decoration bounds
    int32_t deco_x = window_x - border_width;
    int32_t deco_y = window_y - title_bar_height - border_width;
    int32_t deco_width = window_width + (2 * border_width);
    int32_t deco_height = window_height + title_bar_height + (2 * border_width);
    
    // Check bounds to prevent drawing outside screen
    if (deco_x < 0 || deco_y < 0 || 
        deco_x + deco_width >= (int32_t)surface->width || 
        deco_y + deco_height >= (int32_t)surface->height) {
        return; // Skip decorations if they would go outside screen bounds
    }
    
    // 1. Draw window border
    // Top border
    stlxgfx_fill_rect(surface, deco_x, deco_y, deco_width, border_width, border_color);
    // Bottom border  
    stlxgfx_fill_rect(surface, deco_x, deco_y + deco_height - border_width, deco_width, border_width, border_color);
    // Left border
    stlxgfx_fill_rect(surface, deco_x, deco_y, border_width, deco_height, border_color);
    // Right border
    stlxgfx_fill_rect(surface, deco_x + deco_width - border_width, deco_y, border_width, deco_height, border_color);
    
    // 2. Draw title bar
    stlxgfx_fill_rect(surface, deco_x + border_width, deco_y + border_width, 
                      deco_width - (2 * border_width), title_bar_height, title_bar_color);
    
    // 3. Draw close button
    int32_t close_x = deco_x + deco_width - border_width - close_button_margin - close_button_size;
    int32_t close_y = deco_y + border_width + close_button_margin;
    
    // Close button background (modern dark with subtle red highlight)
    stlxgfx_fill_rect(surface, close_x, close_y, close_button_size, close_button_size, is_focused ? close_button_hover : close_button_bg);
    
    // Draw modern X in close button (clean lines)
    const int x_thickness = 1;
    const int x_size = 6;  // Smaller, more refined X
    int32_t x_center_x = close_x + close_button_size / 2;
    int32_t x_center_y = close_y + close_button_size / 2;
    
    // Diagonal line from top-left to bottom-right (thinner, more precise)
    for (int i = 0; i < x_size; i++) {
        stlxgfx_fill_rect(surface, 
                          x_center_x - x_size/2 + i, 
                          x_center_y - x_size/2 + i, 
                          x_thickness, x_thickness, close_button_x_color);
        stlxgfx_fill_rect(surface, 
                          x_center_x - x_size/2 + i + 1, 
                          x_center_y - x_size/2 + i, 
                          x_thickness, x_thickness, close_button_x_color);
    }
    
    // Diagonal line from top-right to bottom-left
    for (int i = 0; i < x_size; i++) {
        stlxgfx_fill_rect(surface, 
                          x_center_x + x_size/2 - i, 
                          x_center_y - x_size/2 + i, 
                          x_thickness, x_thickness, close_button_x_color);
        stlxgfx_fill_rect(surface, 
                          x_center_x + x_size/2 - i - 1, 
                          x_center_y - x_size/2 + i, 
                          x_thickness, x_thickness, close_button_x_color);
    }
    
    // 4. Draw window title (simple text - using window ID for now)
    char title_text[32];
    snprintf(title_text, sizeof(title_text), "Window %u", window_id);
    
    // Calculate text position (left-aligned with some padding)
    int32_t text_x = deco_x + border_width + 10;
    int32_t text_y = deco_y + border_width + (title_bar_height / 2) - 8; // Center vertically
    
    // Draw title text (if graphics context supports text rendering)
    if (compositor->gfx_ctx) {
        stlxgfx_render_text(compositor->gfx_ctx, surface, title_text, 
                           text_x, text_y, 14, title_text_color);
    }
}
