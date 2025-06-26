#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "stlxgfx/internal/stlxgfx_event_dm.h"
#include "stlxgfx/internal/stlxgfx_event_ring.h"
#include "stlxgfx/internal/stlxgfx_event_types.h"

int stlxgfx_dm_create_event_ring_shm(stlxgfx_context_t* ctx,
                                     shm_handle_t* out_shm_handle,
                                     stlxgfx_event_ring_t** out_ring) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Event ring SHM creation only available in Display Manager mode\n");
        return -1;
    }
    
    if (!out_shm_handle || !out_ring) {
        printf("STLXGFX: Invalid output parameters for event ring SHM\n");
        return -1;
    }
    
    // Calculate memory requirements (page-aligned)
    size_t ring_size = stlxgfx_event_ring_get_size();
    size_t page_size = 4096; // Standard page size
    size_t aligned_size = (ring_size + page_size - 1) & ~(page_size - 1);
    
    // Create shared memory with unique name
    char shm_name[64];
    static uint32_t ring_counter = 0;
    snprintf(shm_name, sizeof(shm_name), "stlxgfx_events_%u_%p", 
             ring_counter++, (void*)ctx);
    
    shm_handle_t shm_handle = stlx_shm_create(shm_name, aligned_size, SHM_READ_WRITE);
    if (shm_handle == 0) {
        printf("STLXGFX: Failed to create event ring shared memory (%zu bytes)\n", aligned_size);
        return -1;
    }
    
    // Map shared memory
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map event ring shared memory\n");
        stlx_shm_destroy(shm_handle);
        return -1;
    }
    
    // Initialize ring buffer structure
    stlxgfx_event_ring_t* ring = (stlxgfx_event_ring_t*)shm_memory;
    if (stlxgfx_event_ring_init(ring) != 0) {
        printf("STLXGFX: Failed to initialize event ring buffer\n");
        stlx_shm_unmap(shm_handle, shm_memory);
        stlx_shm_destroy(shm_handle);
        return -1;
    }
    
    // Return results
    *out_shm_handle = shm_handle;
    *out_ring = ring;
    
    printf("STLXGFX: Created event ring SHM '%s' (handle: %lu, size: %zu)\n", 
           shm_name, shm_handle, aligned_size);
    
    return 0;
}

int stlxgfx_dm_destroy_event_ring_shm(stlxgfx_context_t* ctx,
                                      shm_handle_t shm_handle,
                                      stlxgfx_event_ring_t* ring) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Event ring SHM destruction only available in Display Manager mode\n");
        return -1;
    }
    
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory (this invalidates ring pointer)
    if (ring) {
        if (stlx_shm_unmap(shm_handle, ring) != 0) {
            printf("STLXGFX: Warning: Failed to unmap event ring shared memory\n");
            // Continue with destruction attempt
        }
    }
    
    // Destroy shared memory handle
    if (stlx_shm_destroy(shm_handle) != 0) {
        printf("STLXGFX: Failed to destroy event ring shared memory handle\n");
        return -1;
    }
    
    printf("STLXGFX: Destroyed event ring SHM (handle: %lu)\n", shm_handle);
    
    return 0;
}

int stlxgfx_dm_queue_event(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* event) {
    if (!ring || !event) {
        return -1;
    }
    
    // Add timestamp if not already set
    stlxgfx_event_t event_with_timestamp = *event;
    if (event_with_timestamp.id == 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            event_with_timestamp.id = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
        }
    }
    
    return stlxgfx_event_ring_write(ring, &event_with_timestamp);
}

int stlxgfx_dm_queue_events(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* events, int num_events) {
    if (!ring || !events || num_events <= 0) {
        return -1;
    }
    
    return stlxgfx_event_ring_write_batch(ring, events, num_events);
}

int stlxgfx_map_event_ring_shm(shm_handle_t shm_handle,
                               stlxgfx_event_ring_t** out_ring) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    if (!out_ring) {
        printf("STLXGFX: Invalid output parameter for event ring mapping\n");
        return -1;
    }
    
    // Map shared memory with read/write access
    void* shm_memory = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (!shm_memory) {
        printf("STLXGFX: Failed to map event ring shared memory\n");
        return -1;
    }
    
    // Get pointer to ring buffer structure
    stlxgfx_event_ring_t* ring = (stlxgfx_event_ring_t*)shm_memory;
    
    // Basic validation - check if structure looks reasonable
    if (ring->header.buffer_size != STLXGFX_EVENT_RING_CAPACITY ||
        ring->header.event_size != sizeof(stlxgfx_event_t) ||
        ring->header.read_index >= STLXGFX_EVENT_RING_CAPACITY ||
        ring->header.write_index >= STLXGFX_EVENT_RING_CAPACITY) {
        printf("STLXGFX: Invalid event ring data in shared memory\n");
        stlx_shm_unmap(shm_handle, shm_memory);
        return -1;
    }
    
    // Return ring pointer
    *out_ring = ring;
    
    return 0;
}

int stlxgfx_unmap_event_ring_shm(shm_handle_t shm_handle,
                                 stlxgfx_event_ring_t* ring) {
    if (shm_handle == 0) {
        printf("STLXGFX: Invalid shared memory handle\n");
        return -1;
    }
    
    // Unmap shared memory
    if (ring) {
        if (stlx_shm_unmap(shm_handle, ring) != 0) {
            printf("STLXGFX: Failed to unmap event ring shared memory\n");
            return -1;
        }
    }
    
    return 0;
}

int stlxgfx_dm_get_event_ring_stats(const stlxgfx_event_ring_t* ring,
                                    uint32_t* overflow_count,
                                    int* available_read,
                                    int* available_write) {
    if (!ring) {
        return -1;
    }
    
    if (overflow_count) {
        *overflow_count = ring->header.overflow_count;
    }
    
    if (available_read) {
        *available_read = stlxgfx_event_ring_available_read(ring);
    }
    
    if (available_write) {
        *available_write = stlxgfx_event_ring_available_write(ring);
    }
    
    return 0;
}

int stlxgfx_dm_reset_event_ring_stats(stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    return stlxgfx_event_ring_reset_stats(ring);
}

int stlxgfx_dm_check_event_ring_health(const stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    // Check basic structure validity
    if (ring->header.buffer_size != STLXGFX_EVENT_RING_CAPACITY) {
        printf("STLXGFX: Event ring health check failed - invalid buffer size\n");
        return 0;
    }
    
    if (ring->header.event_size != sizeof(stlxgfx_event_t)) {
        printf("STLXGFX: Event ring health check failed - invalid event size\n");
        return 0;
    }
    
    // Check index bounds
    if (ring->header.read_index >= STLXGFX_EVENT_RING_CAPACITY) {
        printf("STLXGFX: Event ring health check failed - read index out of bounds\n");
        return 0;
    }
    
    if (ring->header.write_index >= STLXGFX_EVENT_RING_CAPACITY) {
        printf("STLXGFX: Event ring health check failed - write index out of bounds\n");
        return 0;
    }
    
    // Check for excessive overflow (might indicate a problem)
    if (ring->header.overflow_count > 1000) {
        printf("STLXGFX: Event ring health check warning - high overflow count: %u\n", 
               ring->header.overflow_count);
        // Don't fail the health check for this, just warn
    }
    
    return 1; // Healthy
}
