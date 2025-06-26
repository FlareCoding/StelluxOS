#ifndef STLXGFX_EVENT_DM_H
#define STLXGFX_EVENT_DM_H

#include <stdint.h>
#include <stddef.h>
#include "stlxgfx_ctx.h"
#include "stlxgfx_event_ring.h"
#include "stlxgfx_event_types.h"
#include <stlibc/ipc/shm.h>

/**
 * Create event ring buffer shared memory for a window (Display Manager only)
 * @param ctx - graphics context (must be in DISPLAY_MANAGER mode)
 * @param out_shm_handle - returns the shared memory handle
 * @param out_ring - returns pointer to the mapped ring buffer
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_create_event_ring_shm(stlxgfx_context_t* ctx,
                                     shm_handle_t* out_shm_handle,
                                     stlxgfx_event_ring_t** out_ring);

/**
 * Destroy event ring buffer shared memory (Display Manager only)
 * @param ctx - graphics context
 * @param shm_handle - shared memory handle to destroy
 * @param ring - pointer to ring buffer (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_destroy_event_ring_shm(stlxgfx_context_t* ctx,
                                      shm_handle_t shm_handle,
                                      stlxgfx_event_ring_t* ring);

/**
 * Queue a single event to a window's ring buffer (Display Manager only)
 * @param ring - ring buffer to write to
 * @param event - event to queue
 * @return 0 on success, -1 if buffer is full (event dropped), negative on other errors
 */
int stlxgfx_dm_queue_event(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* event);

/**
 * Queue multiple events to a window's ring buffer (Display Manager only)
 * @param ring - ring buffer to write to
 * @param events - array of events to queue
 * @param num_events - number of events to queue
 * @return number of events actually queued, negative on error
 */
int stlxgfx_dm_queue_events(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* events, int num_events);

/**
 * Map event ring buffer shared memory into application address space
 * @param shm_handle - shared memory handle from display manager
 * @param out_ring - returns pointer to the mapped ring buffer
 * @return 0 on success, negative on error
 */
int stlxgfx_map_event_ring_shm(shm_handle_t shm_handle,
                               stlxgfx_event_ring_t** out_ring);

/**
 * Unmap event ring buffer shared memory from application address space
 * @param shm_handle - shared memory handle
 * @param ring - pointer to ring buffer (will be invalidated)
 * @return 0 on success, negative on error
 */
int stlxgfx_unmap_event_ring_shm(shm_handle_t shm_handle,
                                 stlxgfx_event_ring_t* ring);

/**
 * Get event ring buffer statistics (Display Manager only)
 * @param ring - ring buffer to get stats from
 * @param overflow_count - returns number of overflow events (dropped events)
 * @param available_read - returns number of events available to read
 * @param available_write - returns number of free slots available to write
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_get_event_ring_stats(const stlxgfx_event_ring_t* ring,
                                    uint32_t* overflow_count,
                                    int* available_read,
                                    int* available_write);

/**
 * Reset event ring buffer statistics (Display Manager only)
 * @param ring - ring buffer to reset stats for
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_reset_event_ring_stats(stlxgfx_event_ring_t* ring);

/**
 * Check if event ring buffer is healthy (Display Manager only)
 * @param ring - ring buffer to check
 * @return 1 if healthy, 0 if unhealthy, negative on error
 */
int stlxgfx_dm_check_event_ring_health(const stlxgfx_event_ring_t* ring);

#endif // STLXGFX_EVENT_DM_H
