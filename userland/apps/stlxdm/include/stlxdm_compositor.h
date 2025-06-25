#ifndef STLXDM_COMPOSITOR_H
#define STLXDM_COMPOSITOR_H

#include <stdint.h>
#include <stlxgfx/stlxgfx.h>
#include <stlxgfx/surface.h>
#include "stlxdm_framebuffer.h"

// Forward declarations
typedef struct stlxgfx_context stlxgfx_context_t;

// Compositor context
typedef struct {
    // Graphics context
    stlxgfx_context_t* gfx_ctx;
    
    // Framebuffer management
    struct gfx_framebuffer_info fb_info;
    stlxgfx_pixel_format_t gop_format;
    uint8_t* framebuffer;
    
    // Compositor surface
    stlxgfx_surface_t* compositor_surface;
    
    // State flags
    int initialized;
} stlxdm_compositor_t;

// Window region types for hit testing
typedef enum {
    WINDOW_REGION_NONE,
    WINDOW_REGION_CLOSE_BUTTON,
    WINDOW_REGION_TITLE_BAR,
    WINDOW_REGION_BORDER,
    WINDOW_REGION_CLIENT_AREA
} window_region_t;

/**
 * Initialize the compositor
 * @param compositor - compositor context to initialize
 * @param gfx_ctx - graphics context
 * @return 0 on success, negative on error
 */
int stlxdm_compositor_init(stlxdm_compositor_t* compositor, stlxgfx_context_t* gfx_ctx);

/**
 * Cleanup the compositor
 * @param compositor - compositor context to cleanup
 */
void stlxdm_compositor_cleanup(stlxdm_compositor_t* compositor);

/**
 * Compose the final frame (window composition)
 * @param compositor - compositor context
 * @param server - server context to access client windows
 * @param cursor_x - cursor X position (-1 to skip cursor rendering)
 * @param cursor_y - cursor Y position (-1 to skip cursor rendering)
 * @param focused_window_id - ID of the currently focused window (0 if none)
 * @return 0 on success, negative on error
 */
int stlxdm_compositor_compose(stlxdm_compositor_t* compositor, void* server, int32_t cursor_x, int32_t cursor_y, uint32_t focused_window_id);

/**
 * Present the composed frame to the framebuffer
 * @param compositor - compositor context
 * @return 0 on success, negative on error
 */
int stlxdm_compositor_present(stlxdm_compositor_t* compositor);

/**
 * Get framebuffer info
 * @param compositor - compositor context
 * @return pointer to framebuffer info, NULL on error
 */
const struct gfx_framebuffer_info* stlxdm_compositor_get_fb_info(const stlxdm_compositor_t* compositor);

/**
 * Get compositor surface
 * @param compositor - compositor context
 * @return pointer to compositor surface, NULL on error
 */
stlxgfx_surface_t* stlxdm_compositor_get_surface(const stlxdm_compositor_t* compositor);

/**
 * Draw cursor at specified position
 * @param compositor - compositor context
 * @param x - cursor X position
 * @param y - cursor Y position
 */
void stlxdm_compositor_draw_cursor(stlxdm_compositor_t* compositor, int32_t x, int32_t y);

/**
 * Draw window decorations (frame, title bar, close button)
 * @param compositor - compositor context
 * @param window_x - window X position
 * @param window_y - window Y position
 * @param window_width - window width
 * @param window_height - window height
 * @param is_focused - whether this window has focus
 * @param title - window title to display
 */
void stlxdm_compositor_draw_window_decorations(stlxdm_compositor_t* compositor, 
                                               int32_t window_x, int32_t window_y,
                                               uint32_t window_width, uint32_t window_height,
                                               int is_focused,
                                               const char* title);

/**
 * Hit test a window to determine which region was clicked
 * @param window - target window
 * @param click_x - screen X coordinate of click
 * @param click_y - screen Y coordinate of click
 * @return window region that was clicked
 */
window_region_t stlxdm_hit_test_window(stlxgfx_window_t* window, int32_t click_x, int32_t click_y);

#endif // STLXDM_COMPOSITOR_H
