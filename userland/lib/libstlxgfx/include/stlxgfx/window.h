#ifndef STLXGFX_WINDOW_H
#define STLXGFX_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/internal/stlxgfx_protocol.h>
#include <stlibc/ipc/shm.h>

typedef struct stlxgfx_context stlxgfx_context_t;

// Window structure (shared between DM and applications)
struct stlxgfx_window {
    // Window properties
    uint32_t window_id;
    uint32_t width, height;
    stlxgfx_pixel_format_t format;
    
    // Shared memory handles (same across processes)
    shm_handle_t sync_shm_handle;     // Synchronization data
    shm_handle_t surface_shm_handle;  // Both front and back surfaces
    
    // Local pointers (different virtual addresses per process)
    stlxgfx_window_sync_t* sync_data;    // Mapped sync memory
    stlxgfx_surface_t* surface0;         // First surface buffer
    stlxgfx_surface_t* surface1;         // Second surface buffer
    
    // State
    int initialized;
};

// Window handle typedef
typedef struct stlxgfx_window stlxgfx_window_t;

/**
 * Create a window
 * @param ctx - graphics context (must be in APPLICATION mode)
 * @param width - window width in pixels
 * @param height - window height in pixels
 * @return window handle or NULL on error
 */
stlxgfx_window_t* stlxgfx_create_window(stlxgfx_context_t* ctx, uint32_t width, uint32_t height);

/**
 * Destroy a window
 * @param ctx - graphics context
 * @param window - window to destroy
 */
void stlxgfx_destroy_window(stlxgfx_context_t* ctx, stlxgfx_window_t* window);

/**
 * Get the active drawing surface for a window (back buffer)
 * @param window - target window
 * @return pointer to back buffer surface, NULL on error
 */
stlxgfx_surface_t* stlxgfx_get_active_surface(stlxgfx_window_t* window);

/**
 * Get the current application surface (for drawing)
 * @param window - target window
 * @return pointer to app surface based on app_buffer_index, NULL on error
 */
stlxgfx_surface_t* stlxgfx_get_app_surface(stlxgfx_window_t* window);

/**
 * Get the current display manager surface (for compositing)
 * @param window - target window
 * @return pointer to DM surface based on dm_buffer_index, NULL on error
 */
stlxgfx_surface_t* stlxgfx_get_dm_surface(stlxgfx_window_t* window);

/**
 * Swap front and back buffers
 * @param window - target window
 * @return 0 on success, negative on error
 */
int stlxgfx_swap_buffers(stlxgfx_window_t* window);

/**
 * Handle window synchronization for display manager (compositor)
 * Should be called before compositing a window to ensure proper synchronization
 * @param window - target window
 * @return 1 if window is ready to composite, 0 if not ready, negative on error
 */
int stlxgfx_dm_sync_window(stlxgfx_window_t* window);

/**
 * Finish window synchronization for display manager (compositor)
 * Should be called after compositing a window to signal completion
 * @param window - target window
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_finish_sync_window(stlxgfx_window_t* window);

#endif // STLXGFX_WINDOW_H
 