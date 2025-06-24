#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/window.h"
#include "stlxgfx/surface.h"
#include "stlxgfx/internal/stlxgfx_ctx.h"
#include "stlxgfx/internal/stlxgfx_comm.h"
#include "stlxgfx/internal/stlxgfx_protocol.h"

#define _POSIX_C_SOURCE 199309L
#include <time.h>

stlxgfx_window_t* stlxgfx_create_window(stlxgfx_context_t* ctx, uint32_t width, uint32_t height) {
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
    
    printf("STLXGFX: Creating window %ux%u\n", width, height);
    
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
        .reserved = {0, 0}
    };
    
    // Send request to display manager
    if (stlxgfx_send_message(ctx->socket_fd, &header, &request) != 0) {
        printf("STLXGFX: Failed to send CREATE_WINDOW_REQUEST\n");
        return NULL;
    }
    
    printf("STLXGFX: Sent CREATE_WINDOW_REQUEST (seq=%u)\n", header.sequence_number);
    
    // Wait for response
    stlxgfx_message_header_t response_header;
    stlxgfx_create_window_response_t response;
    
    if (stlxgfx_receive_message(ctx->socket_fd, &response_header, &response, sizeof(response)) != 0) {
        printf("STLXGFX: Failed to receive CREATE_WINDOW_RESPONSE\n");
        return NULL;
    }
    
    printf("STLXGFX: Received response type=%u, seq=%u\n", 
           response_header.message_type, response_header.sequence_number);
    
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
    
    printf("STLXGFX: Window created successfully! ID=%u, sync_shm=%lu, surface_shm=%lu\n", 
           response.window_id, response.sync_shm_handle, response.surface_shm_handle);
    
    // Validate that we received valid SHM handles
    if (response.sync_shm_handle == 0 || response.surface_shm_handle == 0) {
        printf("STLXGFX: Invalid SHM handles in response\n");
        return NULL;
    }
    
    // Map window sync shared memory
    stlxgfx_window_sync_t* sync_data;
    if (stlxgfx_map_window_sync_shm(response.sync_shm_handle, &sync_data) != 0) {
        printf("STLXGFX: Failed to map window sync SHM\n");
        return NULL;
    }
    
    // Map surface pair shared memory
    stlxgfx_surface_t* surface0;
    stlxgfx_surface_t* surface1;
    if (stlxgfx_map_shared_surface_pair(response.surface_shm_handle, &surface0, &surface1) != 0) {
        printf("STLXGFX: Failed to map surface pair SHM\n");
        // Clean up sync mapping
        stlxgfx_unmap_window_sync_shm(response.sync_shm_handle, sync_data);
        return NULL;
    }
    
    // Create window structure
    stlxgfx_window_t* window = malloc(sizeof(stlxgfx_window_t));
    if (!window) {
        printf("STLXGFX: Failed to allocate window structure\n");
        // Clean up mappings
        stlxgfx_unmap_shared_surface_pair(response.surface_shm_handle, surface0, surface1);
        stlxgfx_unmap_window_sync_shm(response.sync_shm_handle, sync_data);
        return NULL;
    }
    
    // Initialize window structure with all mapped data
    window->window_id = response.window_id;
    window->width = width;
    window->height = height;
    window->format = (stlxgfx_pixel_format_t)response.surface_format;
    window->sync_shm_handle = response.sync_shm_handle;
    window->surface_shm_handle = response.surface_shm_handle;
    window->sync_data = sync_data;
    
    // Store surface pointers directly - no front/back assignment needed
    // The app_buffer_index and dm_buffer_index in sync_data determine which surface each side uses
    window->surface0 = surface0;
    window->surface1 = surface1;
    
    window->initialized = 1;
    
    printf("STLXGFX: Window %ux%u mapped successfully (app_buf=%u, dm_buf=%u, visible=%u)\n",
           width, height, sync_data->app_buffer_index, sync_data->dm_buffer_index, sync_data->window_visible);
    
    return window;
}

void stlxgfx_destroy_window(stlxgfx_context_t* ctx, stlxgfx_window_t* window) {
    if (!ctx || !window) {
        return;
    }
    
    printf("STLXGFX: Destroying window ID=%u\n", window->window_id);
    
    // Clean up shared memory mappings if they exist
    if (window->initialized) {
        if (window->surface_shm_handle != 0 && window->surface0) {
            stlxgfx_unmap_shared_surface_pair(window->surface_shm_handle, 
                                              window->surface0, window->surface1);
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
    
    // App draws to the surface indicated by app_buffer_index
    if (window->sync_data->app_buffer_index == 0) {
        return window->surface0;
    } else {
        return window->surface1;
    }
}

stlxgfx_surface_t* stlxgfx_get_dm_surface(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return NULL;
    }
    
    // DM reads from the surface indicated by dm_buffer_index
    if (window->sync_data->dm_buffer_index == 0) {
        return window->surface0;
    } else {
        return window->surface1;
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
    
    printf("STLXGFX: Starting buffer swap for window ID=%u\n", window->window_id);
    
    // Signal that the application has finished drawing this frame
    window->sync_data->app_frame_ready = 1;
    printf("STLXGFX: Set app_frame_ready=1, waiting for compositor to consume...\n");
    
    // Wait for display manager to detect and start consuming the frame
    // The DM will set dm_frame_consumed=0 AND reset app_frame_ready=0 when it starts processing
    int timeout_count = 0;
    const int max_timeout = 4000; // 4 seconds at 1ms per loop iteration
    
    // Wait for DM to start consuming (dm_frame_consumed becomes 0)
    // OR for DM to reset our app_frame_ready flag (indicating it saw and processed our request)
    while (window->sync_data->dm_frame_consumed == 1 && window->sync_data->app_frame_ready == 1) {
        if (timeout_count++ >= max_timeout) {
            printf("STLXGFX: Timeout waiting for DM to start consuming frame (window ID=%u)\n", window->window_id);
            window->sync_data->app_frame_ready = 0; // Reset on timeout
            return -2; // Timeout error
        }
        
        // Small delay to prevent busy spinning - use 1ms as requested
        struct timespec delay = { 0, 1000000 }; // 1ms
        nanosleep(&delay, NULL);
    }
    
    // Check the reason we exited the loop
    if (window->sync_data->app_frame_ready == 0) {
        printf("STLXGFX: DM acknowledged frame, waiting for composition to complete...\n");
    } else {
        printf("STLXGFX: DM started consuming frame, waiting for completion...\n");
    }
    
    // Now wait for DM to finish consuming the frame (dm_frame_consumed goes back to 1)
    timeout_count = 0;
    while (window->sync_data->dm_frame_consumed == 0) {
        if (timeout_count++ >= max_timeout) {
            printf("STLXGFX: Timeout waiting for DM to finish consuming frame (window ID=%u)\n", window->window_id);
            return -2; // Timeout error
        }
        
        // Small delay to prevent busy spinning
        struct timespec delay = { 0, 1000000 }; // 1ms
        nanosleep(&delay, NULL);
    }
    
    printf("STLXGFX: DM finished consuming frame\n");
    
    // Update buffer indices in sync structure - this is the actual "swap"
    // No need to swap local pointers, just update the shared indices
    window->sync_data->app_buffer_index = 1 - window->sync_data->app_buffer_index;
    window->sync_data->dm_buffer_index = 1 - window->sync_data->dm_buffer_index;
    
    // Reset frame ready flag for next frame
    window->sync_data->app_frame_ready = 0;
    
    printf("STLXGFX: Buffer swap complete (app_buf=%u, dm_buf=%u)\n", 
           window->sync_data->app_buffer_index, window->sync_data->dm_buffer_index);
    
    return 0;
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
    
    // Check if application has a new frame ready
    if (window->sync_data->app_frame_ready == 0) {
        // No new frame available, nothing to composite
        return 0;
    }
    
    // Signal that DM is starting to consume the frame
    window->sync_data->dm_frame_consumed = 0;
    
    // IMPORTANT: Reset app_frame_ready immediately to prevent double-processing
    // The application is waiting for dm_frame_consumed to become 0, which just happened
    window->sync_data->app_frame_ready = 0;
    
    printf("STLXGFX_DM: Starting frame consumption for window ID=%u (app_buf=%u, dm_buf=%u)\n", 
           window->window_id, window->sync_data->app_buffer_index, window->sync_data->dm_buffer_index);
    
    return 1; // Ready to composite
}

int stlxgfx_dm_finish_sync_window(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->sync_data) {
        return -1;
    }
    
    // Signal that DM has finished consuming the frame
    window->sync_data->dm_frame_consumed = 1;
    
    printf("STLXGFX_DM: Finished frame consumption for window ID=%u\n", window->window_id);
    
    return 0;
}


 