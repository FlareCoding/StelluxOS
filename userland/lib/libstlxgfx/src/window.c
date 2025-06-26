#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/window.h"
#include "stlxgfx/surface.h"
#include "stlxgfx/internal/stlxgfx_ctx.h"
#include "stlxgfx/internal/stlxgfx_comm.h"
#include "stlxgfx/internal/stlxgfx_protocol.h"
#include "stlxgfx/internal/stlxgfx_event_dm.h"

#define _POSIX_C_SOURCE 199309L
#include <time.h>

// Forward declaration for internal event registration
extern int stlxgfx_register_window_for_events(stlxgfx_window_t* window);
extern int stlxgfx_unregister_window_from_events(stlxgfx_window_t* window);

stlxgfx_window_t* stlxgfx_create_window(stlxgfx_context_t* ctx, uint32_t width, uint32_t height, 
                                       int32_t posx, int32_t posy, const char* title) {
    if (!ctx || !ctx->initialized) {
        printf("STLXGFX: Invalid context\n");
        return NULL;
    }
    
    if (ctx->mode != STLXGFX_MODE_APPLICATION) {
        printf("STLXGFX: Window creation only available in application mode\n");
        return NULL;
    }
    
    if (!ctx->connected_to_dm) {
        printf("STLXGFX: Not connected to display manager\n");
        return NULL;
    }
    
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        printf("STLXGFX: Invalid window dimensions: %ux%u\n", width, height);
        return NULL;
    }
    
    // Prepare CREATE_WINDOW_REQUEST
    stlxgfx_message_header_t header = {
        .protocol_version = STLXGFX_PROTOCOL_VERSION,
        .message_type = STLXGFX_MSG_CREATE_WINDOW_REQUEST,
        .sequence_number = ctx->next_sequence_number++,
        .payload_size = sizeof(stlxgfx_create_window_request_t),
        .flags = 0
    };
    
    stlxgfx_create_window_request_t request = {
        .width = width,
        .height = height,
        .posx = posx,
        .posy = posy,
        .title_length = 0,
        .title = {0}
    };
    
    // Handle title
    if (title && title[0] != '\0') {
        size_t title_len = strlen(title);
        if (title_len >= sizeof(request.title)) {
            title_len = sizeof(request.title) - 1; // Leave room for null terminator
        }
        memcpy(request.title, title, title_len);
        request.title[title_len] = '\0';
        request.title_length = title_len;
    }
    
    // Send request to display manager
    if (stlxgfx_send_message(ctx->socket_fd, &header, &request) != 0) {
        printf("STLXGFX: Failed to send CREATE_WINDOW_REQUEST\n");
        return NULL;
    }
    
    // Wait for response
    stlxgfx_message_header_t response_header;
    stlxgfx_create_window_response_t response;
    
    if (stlxgfx_receive_message(ctx->socket_fd, &response_header, &response, sizeof(response)) != 0) {
        printf("STLXGFX: Failed to receive CREATE_WINDOW_RESPONSE\n");
        return NULL;
    }
    
    // Validate response
    if (response_header.message_type == STLXGFX_MSG_ERROR_RESPONSE) {
        stlxgfx_error_response_t* error = (stlxgfx_error_response_t*)&response;
        printf("STLXGFX: Error response: %s\n", error->error_message);
        return NULL;
    }
    
    if (response_header.message_type != STLXGFX_MSG_CREATE_WINDOW_RESPONSE) {
        printf("STLXGFX: Unexpected response type: %u\n", response_header.message_type);
        return NULL;
    }
    
    if (response_header.sequence_number != header.sequence_number) {
        printf("STLXGFX: Sequence number mismatch: sent %u, got %u\n", 
               header.sequence_number, response_header.sequence_number);
        return NULL;
    }
    
    if (response.result_code != STLXGFX_ERROR_SUCCESS) {
        printf("STLXGFX: Window creation failed: %d\n", response.result_code);
        return NULL;
    }
    
    // Validate that we received valid SHM handles
    if (response.sync_shm_handle == 0 || response.surface_shm_handle == 0 || response.event_shm_handle == 0) {
        printf("STLXGFX: Invalid SHM handles in response\n");
        return NULL;
    }
    
    // Map window sync shared memory
    stlxgfx_window_sync_t* sync_data;
    if (stlxgfx_map_window_sync_shm(response.sync_shm_handle, &sync_data) != 0) {
        printf("STLXGFX: Failed to map window sync SHM\n");
        return NULL;
    }
    
    // Map surface set shared memory
    stlxgfx_surface_t* surface0;
    stlxgfx_surface_t* surface1;
    stlxgfx_surface_t* surface2;
    if (stlxgfx_map_shared_surface_set(response.surface_shm_handle, &surface0, &surface1, &surface2) != 0) {
        printf("STLXGFX: Failed to map surface set SHM\n");
        // Clean up sync mapping
        stlxgfx_unmap_window_sync_shm(response.sync_shm_handle, sync_data);
        return NULL;
    }
    
    // Map event ring buffer shared memory
    stlxgfx_event_ring_t* event_ring;
    if (stlxgfx_map_event_ring_shm(response.event_shm_handle, &event_ring) != 0) {
        printf("STLXGFX: Failed to map event ring SHM\n");
        // Clean up surface set mapping
        stlxgfx_unmap_shared_surface_set(response.surface_shm_handle, surface0, surface1, surface2);
        // Clean up sync mapping
        stlxgfx_unmap_window_sync_shm(response.sync_shm_handle, sync_data);
        return NULL;
    }
    
    // Create window structure
    stlxgfx_window_t* window = malloc(sizeof(stlxgfx_window_t));
    if (!window) {
        printf("STLXGFX: Failed to allocate window structure\n");
        // Clean up all mappings
        stlxgfx_unmap_event_ring_shm(response.event_shm_handle, event_ring);
        stlxgfx_unmap_shared_surface_set(response.surface_shm_handle, surface0, surface1, surface2);
        stlxgfx_unmap_window_sync_shm(response.sync_shm_handle, sync_data);
        return NULL;
    }
    
    // Initialize window structure with all mapped data
    window->window_id = response.window_id;
    window->width = width;
    window->height = height;
    window->posx = posx;
    window->posy = posy;
    if (request.title_length > 0) {
        memcpy(window->title, request.title, request.title_length);
        window->title[request.title_length] = '\0';
    } else {
        window->title[0] = '\0';
    }
    window->format = (stlxgfx_pixel_format_t)response.surface_format;
    window->sync_shm_handle = response.sync_shm_handle;
    window->surface_shm_handle = response.surface_shm_handle;
    window->event_shm_handle = response.event_shm_handle;
    window->sync_data = sync_data;
    
    // Store surface pointers directly - triple buffering uses indices to determine which surface each side uses
    window->surface0 = surface0;
    window->surface1 = surface1;
    window->surface2 = surface2;
    
    // Store event ring buffer pointer
    window->event_ring = event_ring;
    
    window->initialized = 1;
    
    // Register this window for event processing
    if (stlxgfx_register_window_for_events(window) != 0) {
        printf("STLXGFX: Warning - Failed to register window for event processing\n");
        // Continue anyway - window creation succeeded
    }
    
    return window;
}

void stlxgfx_destroy_window(stlxgfx_context_t* ctx, stlxgfx_window_t* window) {
    if (!ctx || !window) {
        return;
    }
    
    // Unregister this window from event processing
    stlxgfx_unregister_window_from_events(window);
    
    // Clean up shared memory mappings if they exist
    if (window->initialized) {
        // Clean up event ring buffer shared memory
        if (window->event_shm_handle != 0 && window->event_ring) {
            stlxgfx_unmap_event_ring_shm(window->event_shm_handle, window->event_ring);
        }
        
        if (window->surface_shm_handle != 0 && window->surface0) {
            stlxgfx_unmap_shared_surface_set(window->surface_shm_handle, 
                                             window->surface0, window->surface1, window->surface2);
        }
        
        if (window->sync_shm_handle != 0 && window->sync_data) {
            stlxgfx_unmap_window_sync_shm(window->sync_shm_handle, window->sync_data);
        }
    }
    
    // TODO: Send DESTROY_WINDOW_REQUEST in later phases
    
    free(window);
}

stlxgfx_surface_t* stlxgfx_get_active_surface(stlxgfx_window_t* window) {
    if (!window || !window->initialized) {
        printf("STLXGFX: Invalid window for get_active_surface\n");
        return NULL;
    }
    
    if (!window->surface0 || !window->surface1 || !window->sync_data) {
        printf("STLXGFX: No surfaces available for window ID=%u\n", window->window_id);
        return NULL;
    }
    
    // Return the surface the application should draw to based on app_buffer_index
    return stlxgfx_get_app_surface(window);
}

stlxgfx_surface_t* stlxgfx_get_app_surface(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return NULL;
    }
    
    // App draws to the surface indicated by back_buffer_index
    uint32_t index = window->sync_data->back_buffer_index;
    if (index == 0) {
        return window->surface0;
    } else if (index == 1) {
        return window->surface1;
    } else {
        return window->surface2;
    }
}

stlxgfx_surface_t* stlxgfx_get_dm_surface(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return NULL;
    }
    
    // DM reads from the surface indicated by front_buffer_index
    uint32_t index = window->sync_data->front_buffer_index;
    if (index == 0) {
        return window->surface0;
    } else if (index == 1) {
        return window->surface1;
    } else {
        return window->surface2;
    }
}

int stlxgfx_swap_buffers(stlxgfx_window_t* window) {
    if (!window || !window->initialized) {
        printf("STLXGFX: Invalid window for swap_buffers\n");
        return -1;
    }
    
    if (!window->sync_data) {
        printf("STLXGFX: No sync data available for window ID=%u\n", window->window_id);
        return -1;
    }
    
    // Check if there's already a swap pending - if so, we can't swap yet
    if (window->sync_data->swap_pending) {
        return -3; // Swap pending error - app should retry later or drop frame
    }
    
    // Move current back buffer to ready position
    window->sync_data->ready_buffer_index = window->sync_data->back_buffer_index;
    
    // Signal that a new frame is ready and a swap is pending
    window->sync_data->frame_ready = 1;
    window->sync_data->swap_pending = 1;
    
    // Find the next free buffer for the app to draw to
    // In triple buffering, we cycle through buffers: 0 -> 1 -> 2 -> 0
    uint32_t next_back = (window->sync_data->back_buffer_index + 1) % 3;
    
    // Make sure the next buffer isn't currently being consumed by DM
    // If DM is consuming the buffer we want to use, we need to pick a different one
    if (window->sync_data->dm_consuming && next_back == window->sync_data->front_buffer_index) {
        // DM is consuming our preferred next buffer, use the other free buffer
        next_back = (next_back + 1) % 3;
    }
    
    // Update to the new back buffer
    window->sync_data->back_buffer_index = next_back;
    
    return 0; // Success - app can immediately start drawing next frame
}

int stlxgfx_can_swap_buffers(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return 0; // Cannot swap
    }
    
    // In triple buffering, we can swap as long as there's no pending swap
    return !window->sync_data->swap_pending;
}

int stlxgfx_dm_sync_window(stlxgfx_window_t* window) {
    if (!window || !window->initialized) {
        printf("STLXGFX_DM: Invalid window for sync\n");
        return -1;
    }
    
    if (!window->sync_data) {
        printf("STLXGFX_DM: No sync data available for window ID=%u\n", window->window_id);
        return -1;
    }
    
    // Check if there's a pending swap to process
    if (window->sync_data->swap_pending && window->sync_data->frame_ready) {
        // Perform the swap: move ready buffer to front
        window->sync_data->front_buffer_index = window->sync_data->ready_buffer_index;
        
        // Clear the swap flags
        window->sync_data->frame_ready = 0;
        window->sync_data->swap_pending = 0;
    }
    
    // Always return 1 to composite the current front buffer
    // In triple buffering, we always have a valid front buffer to display
    
    // Signal that DM is consuming the front buffer
    window->sync_data->dm_consuming = 1;
    
    return 1; // Ready to composite
}

int stlxgfx_dm_finish_sync_window(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return -1;
    }
    
    // Signal that DM has finished consuming the frame
    window->sync_data->dm_consuming = 0;
    
    return 0;
}


 