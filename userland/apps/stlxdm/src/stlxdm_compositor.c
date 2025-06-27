#include "stlxdm_compositor.h"
#include "stlxdm_server.h"
#include <stdio.h>
#include <string.h>
#include <stlxgfx/internal/stlxgfx_dm.h>
#include <stlxgfx/window.h>

// Window decoration constants
#define WINDOW_CLOSE_BUTTON_SIZE    14
#define WINDOW_CLOSE_BUTTON_MARGIN  9
#define WINDOW_CLOSE_BUTTON_X_SIZE  6
#define WINDOW_CLOSE_BUTTON_X_THICKNESS 1

int stlxdm_compositor_init(stlxdm_compositor_t* compositor, stlxgfx_context_t* gfx_ctx, stlxdm_hud_t* hud) {
    if (!compositor || !gfx_ctx) {
        printf("[STLXDM_COMPOSITOR] ERROR: Invalid parameters for compositor init\n");
        return -1;
    }
    
    // Initialize compositor context
    memset(compositor, 0, sizeof(stlxdm_compositor_t));
    compositor->gfx_ctx = gfx_ctx;
    compositor->hud = hud;
    
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
        
        const int window_x = window->posx;
        const int window_y = window->posy;
        
        // Check if window has any visible parts (allow partial off-screen rendering)
        // Only skip if window is completely off-screen
        if (window_x + (int32_t)window->width <= 0 || 
            window_y + (int32_t)window->height <= 0 ||
            window_x >= (int32_t)compositor->compositor_surface->width ||
            window_y >= (int32_t)compositor->compositor_surface->height) {
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
        
        // Blit the window surface to the compositor surface at window's position
        // We always composite the DM surface, regardless of whether there's a new frame
        int blit_result = stlxgfx_blit_surface(
            dm_surface, 0, 0,                       // Source: DM surface
            compositor->compositor_surface, window_x, window_y,  // Dest: window's position
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
            is_focused,
            window->title
        );
    }
    
    // Render HUD
    if (compositor->hud) {
        if (stlxdm_hud_render(compositor->hud, compositor->compositor_surface) < 0) {
            printf("[STLXDM_COMPOSITOR] Error rendering HUD\n");
        }
    }
    
    // Render cursor if valid position provided (on top of everything)
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
    
    // Calculate clipping bounds - only draw visible parts
    int start_x = (x < 0) ? -x : 0;
    int start_y = (y < 0) ? -y : 0;
    int end_x = (x + cursor_width > (int32_t)surface->width) ? cursor_width - ((x + cursor_width) - (int32_t)surface->width) : cursor_width;
    int end_y = (y + cursor_height > (int32_t)surface->height) ? cursor_height - ((y + cursor_height) - (int32_t)surface->height) : cursor_height;
    
    // Check if cursor is completely off-screen
    if (start_x >= cursor_width || start_y >= cursor_height || end_x <= 0 || end_y <= 0) {
        return;
    }
    
    // First pass: Draw shadow (offset by 1 pixel down and right)
    for (int row = start_y; row < end_y; row++) {
        for (int col = start_x; col < end_x; col++) {
            char pixel = cursor_shape[row][col];
            if (pixel == 'X' || pixel == '.') {
                int shadow_x = x + col + 1;
                int shadow_y = y + row + 1;
                if (shadow_x >= 0 && shadow_x < (int32_t)surface->width && 
                    shadow_y >= 0 && shadow_y < (int32_t)surface->height) {
                    stlxgfx_draw_pixel(surface, shadow_x, shadow_y, shadow);
                }
            }
        }
    }
    
    // Second pass: Draw main cursor
    for (int row = start_y; row < end_y; row++) {
        for (int col = start_x; col < end_x; col++) {
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
                                               int is_focused,
                                               const char* title) {
    if (!compositor || !compositor->initialized || !compositor->compositor_surface) {
        return;
    }
    
    stlxgfx_surface_t* surface = compositor->compositor_surface;
    
    // Modern dark color scheme
    uint32_t border_color = is_focused ? 0xFF2D2D30 : 0xFF1E1E1E;  // Dark gray borders
    uint32_t title_bar_color = is_focused ? 0xFF3C3C3C : 0xFF2B2B2B;  // Dark title bars
    uint32_t title_text_color = is_focused ? 0xFFFFFFFF : 0xFFBBBBBB;  // White/light gray text
    uint32_t close_button_bg = 0xFF404040;   // Dark gray close button background
    uint32_t close_button_hover = 0xFFE81123;  // Red on hover (we'll use this as default for now)
    uint32_t close_button_x_color = 0xFFFFFFFF;  // White X
    
    // Calculate decoration bounds
    int32_t deco_x = window_x - WINDOW_BORDER_WIDTH;
    int32_t deco_y = window_y - WINDOW_TITLE_BAR_HEIGHT - WINDOW_BORDER_WIDTH;
    int32_t deco_width = window_width + (2 * WINDOW_BORDER_WIDTH);
    int32_t deco_height = window_height + WINDOW_TITLE_BAR_HEIGHT + (2 * WINDOW_BORDER_WIDTH);
    
    // Clip decoration bounds to surface
    int32_t clip_x = 0;
    int32_t clip_y = 0;
    int32_t clip_w = (int32_t)surface->width;
    int32_t clip_h = (int32_t)surface->height;
    
    int32_t draw_x = deco_x < clip_x ? clip_x : deco_x;
    int32_t draw_y = deco_y < clip_y ? clip_y : deco_y;
    int32_t draw_w = (deco_x + deco_width > clip_w) ? (clip_w - draw_x) : (deco_x + deco_width - draw_x);
    int32_t draw_h = (deco_y + deco_height > clip_h) ? (clip_h - draw_y) : (deco_y + deco_height - draw_y);
    if (draw_w <= 0 || draw_h <= 0) return;
    
    // 1. Draw window border (clipped)
    // Top border
    stlxgfx_fill_rect(surface, draw_x, draw_y, draw_w, WINDOW_BORDER_WIDTH, border_color);
    // Bottom border
    stlxgfx_fill_rect(surface, draw_x, draw_y + draw_h - WINDOW_BORDER_WIDTH, draw_w, WINDOW_BORDER_WIDTH, border_color);
    // Left border
    stlxgfx_fill_rect(surface, draw_x, draw_y, WINDOW_BORDER_WIDTH, draw_h, border_color);
    // Right border
    stlxgfx_fill_rect(surface, draw_x + draw_w - WINDOW_BORDER_WIDTH, draw_y, WINDOW_BORDER_WIDTH, draw_h, border_color);
    
    // 2. Draw title bar (clipped)
    int32_t titlebar_x = draw_x + WINDOW_BORDER_WIDTH;
    int32_t titlebar_y = draw_y + WINDOW_BORDER_WIDTH;
    int32_t titlebar_w = draw_w - (2 * WINDOW_BORDER_WIDTH);
    int32_t titlebar_h = WINDOW_TITLE_BAR_HEIGHT;
    if (titlebar_w > 0 && titlebar_h > 0 && titlebar_y >= 0 && titlebar_y < (int32_t)surface->height) {
        stlxgfx_fill_rect(surface, titlebar_x, titlebar_y, titlebar_w, titlebar_h, title_bar_color);
    }
    
    // 3. Draw close button (clipped)
    int32_t close_x = deco_x + deco_width - WINDOW_BORDER_WIDTH - WINDOW_CLOSE_BUTTON_MARGIN - WINDOW_CLOSE_BUTTON_SIZE;
    int32_t close_y = deco_y + WINDOW_BORDER_WIDTH + WINDOW_CLOSE_BUTTON_MARGIN;
    if (close_x + WINDOW_CLOSE_BUTTON_SIZE > 0 && close_x < (int32_t)surface->width &&
        close_y + WINDOW_CLOSE_BUTTON_SIZE > 0 && close_y < (int32_t)surface->height) {
        stlxgfx_fill_rect(surface, close_x, close_y, WINDOW_CLOSE_BUTTON_SIZE, WINDOW_CLOSE_BUTTON_SIZE, is_focused ? close_button_hover : close_button_bg);
        // Draw modern X in close button (clean lines)
        int32_t x_center_x = close_x + WINDOW_CLOSE_BUTTON_SIZE / 2;
        int32_t x_center_y = close_y + WINDOW_CLOSE_BUTTON_SIZE / 2;
        for (int i = 0; i < WINDOW_CLOSE_BUTTON_X_SIZE; i++) {
            stlxgfx_fill_rect(surface, 
                              x_center_x - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i, 
                              x_center_y - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i, 
                              WINDOW_CLOSE_BUTTON_X_THICKNESS, WINDOW_CLOSE_BUTTON_X_THICKNESS, close_button_x_color);
            stlxgfx_fill_rect(surface, 
                              x_center_x - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i + 1, 
                              x_center_y - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i, 
                              WINDOW_CLOSE_BUTTON_X_THICKNESS, WINDOW_CLOSE_BUTTON_X_THICKNESS, close_button_x_color);
        }
        for (int i = 0; i < WINDOW_CLOSE_BUTTON_X_SIZE; i++) {
            stlxgfx_fill_rect(surface, 
                              x_center_x + WINDOW_CLOSE_BUTTON_X_SIZE/2 - i, 
                              x_center_y - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i, 
                              WINDOW_CLOSE_BUTTON_X_THICKNESS, WINDOW_CLOSE_BUTTON_X_THICKNESS, close_button_x_color);
            stlxgfx_fill_rect(surface, 
                              x_center_x + WINDOW_CLOSE_BUTTON_X_SIZE/2 - i - 1, 
                              x_center_y - WINDOW_CLOSE_BUTTON_X_SIZE/2 + i, 
                              WINDOW_CLOSE_BUTTON_X_THICKNESS, WINDOW_CLOSE_BUTTON_X_THICKNESS, close_button_x_color);
        }
    }
    
    // 4. Draw window title (clipped)
    const char* title_to_display = title && title[0] != '\0' ? title : "Untitled";
    int32_t text_x = deco_x + WINDOW_BORDER_WIDTH + 10;
    int32_t text_y = deco_y + WINDOW_BORDER_WIDTH + (WINDOW_TITLE_BAR_HEIGHT / 2) - 8; // Center vertically
    if (compositor->gfx_ctx && text_x >= 0 && text_x < (int32_t)surface->width && text_y >= 0 && text_y < (int32_t)surface->height) {
        stlxgfx_render_text(compositor->gfx_ctx, surface, title_to_display, 
                           text_x, text_y, 14, title_text_color);
    }
}

window_region_t stlxdm_hit_test_window(stlxgfx_window_t* window, int32_t click_x, int32_t click_y) {
    if (!window || !window->initialized) {
        return WINDOW_REGION_NONE;
    }
    
    // Calculate decoration bounds
    int32_t deco_x = window->posx - WINDOW_BORDER_WIDTH;
    int32_t deco_y = window->posy - WINDOW_TITLE_BAR_HEIGHT - WINDOW_BORDER_WIDTH;
    int32_t deco_width = window->width + (2 * WINDOW_BORDER_WIDTH);
    int32_t deco_height = window->height + WINDOW_TITLE_BAR_HEIGHT + (2 * WINDOW_BORDER_WIDTH);
    
    // Check if click is within window bounds at all
    if (click_x < deco_x || click_x >= deco_x + deco_width ||
        click_y < deco_y || click_y >= deco_y + deco_height) {
        return WINDOW_REGION_NONE;
    }
    
    // Check close button first (highest priority)
    int32_t close_x = deco_x + deco_width - WINDOW_BORDER_WIDTH - WINDOW_CLOSE_BUTTON_MARGIN - WINDOW_CLOSE_BUTTON_SIZE;
    int32_t close_y = deco_y + WINDOW_BORDER_WIDTH + WINDOW_CLOSE_BUTTON_MARGIN;
    
    if (click_x >= close_x && click_x < close_x + WINDOW_CLOSE_BUTTON_SIZE &&
        click_y >= close_y && click_y < close_y + WINDOW_CLOSE_BUTTON_SIZE) {
        return WINDOW_REGION_CLOSE_BUTTON;
    }
    
    // Check borders
    // Top border
    if (click_y >= deco_y && click_y < deco_y + WINDOW_BORDER_WIDTH) {
        return WINDOW_REGION_BORDER;
    }
    // Bottom border
    if (click_y >= deco_y + deco_height - WINDOW_BORDER_WIDTH && click_y < deco_y + deco_height) {
        return WINDOW_REGION_BORDER;
    }
    // Left border
    if (click_x >= deco_x && click_x < deco_x + WINDOW_BORDER_WIDTH) {
        return WINDOW_REGION_BORDER;
    }
    // Right border
    if (click_x >= deco_x + deco_width - WINDOW_BORDER_WIDTH && click_x < deco_x + deco_width) {
        return WINDOW_REGION_BORDER;
    }
    
    // Check title bar (after borders)
    int32_t title_x = deco_x + WINDOW_BORDER_WIDTH;
    int32_t title_y = deco_y + WINDOW_BORDER_WIDTH;
    int32_t title_width = deco_width - (2 * WINDOW_BORDER_WIDTH);
    int32_t title_height = WINDOW_TITLE_BAR_HEIGHT;
    
    if (click_x >= title_x && click_x < title_x + title_width &&
        click_y >= title_y && click_y < title_y + title_height) {
        return WINDOW_REGION_TITLE_BAR;
    }
    
    // If we get here, it's the client area
    return WINDOW_REGION_CLIENT_AREA;
}
