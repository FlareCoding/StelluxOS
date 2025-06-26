#ifndef STLXGFX_EVENT_RING_H
#define STLXGFX_EVENT_RING_H

#include <stdint.h>
#include <stddef.h>
#include "stlxgfx_event_types.h"

// Ring buffer configuration
#define STLXGFX_EVENT_RING_CAPACITY     64  // Fixed capacity: 64 events
#define STLXGFX_CACHE_LINE_SIZE         64  // Cache line size for alignment

// Cache-line aligned ring buffer header
typedef struct {
    volatile uint32_t read_index;     // Consumer index (DM reads from here)
    volatile uint32_t write_index;    // Producer index (DM writes to here)
    uint32_t buffer_size;             // Fixed at STLXGFX_EVENT_RING_CAPACITY
    uint32_t event_size;              // sizeof(stlxgfx_event_t)
    uint32_t overflow_count;          // Statistics: number of dropped events
    uint32_t reserved[11];            // Padding to 64 bytes for cache line alignment
} __attribute__((aligned(STLXGFX_CACHE_LINE_SIZE))) stlxgfx_event_ring_header_t;

// Ring buffer structure (contiguous header + event array)
typedef struct {
    stlxgfx_event_ring_header_t header;                     // Ring buffer header
    stlxgfx_event_t events[STLXGFX_EVENT_RING_CAPACITY];    // Fixed-size event array
} __attribute__((aligned(STLXGFX_CACHE_LINE_SIZE))) stlxgfx_event_ring_t;

/**
 * Initialize a ring buffer structure
 * @param ring - ring buffer structure to initialize
 * @return 0 on success, negative on error
 */
int stlxgfx_event_ring_init(stlxgfx_event_ring_t* ring);

/**
 * Check if ring buffer is empty (no events to read)
 * @param ring - ring buffer to check
 * @return 1 if empty, 0 if not empty, negative on error
 */
int stlxgfx_event_ring_is_empty(const stlxgfx_event_ring_t* ring);

/**
 * Check if ring buffer is full (no space to write)
 * @param ring - ring buffer to check
 * @return 1 if full, 0 if not full, negative on error
 */
int stlxgfx_event_ring_is_full(const stlxgfx_event_ring_t* ring);

/**
 * Get number of events available to read
 * @param ring - ring buffer to check
 * @return number of events available, negative on error
 */
int stlxgfx_event_ring_available_read(const stlxgfx_event_ring_t* ring);

/**
 * Get number of free slots available to write
 * @param ring - ring buffer to check
 * @return number of free slots, negative on error
 */
int stlxgfx_event_ring_available_write(const stlxgfx_event_ring_t* ring);

/**
 * Read a single event from the ring buffer (consumer operation)
 * @param ring - ring buffer to read from
 * @param event - buffer to store the event
 * @return 0 on success, -1 if buffer is empty, negative on other errors
 */
int stlxgfx_event_ring_read(stlxgfx_event_ring_t* ring, stlxgfx_event_t* event);

/**
 * Write a single event to the ring buffer (producer operation)
 * @param ring - ring buffer to write to
 * @param event - event to write
 * @return 0 on success, -1 if buffer is full, negative on other errors
 */
int stlxgfx_event_ring_write(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* event);

/**
 * Read multiple events from the ring buffer (consumer operation)
 * @param ring - ring buffer to read from
 * @param events - buffer to store the events
 * @param max_events - maximum number of events to read
 * @return number of events actually read, negative on error
 */
int stlxgfx_event_ring_read_batch(stlxgfx_event_ring_t* ring, stlxgfx_event_t* events, int max_events);

/**
 * Write multiple events to the ring buffer (producer operation)
 * @param ring - ring buffer to write to
 * @param events - array of events to write
 * @param num_events - number of events to write
 * @return number of events actually written, negative on error
 */
int stlxgfx_event_ring_write_batch(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* events, int num_events);

/**
 * Get ring buffer statistics
 * @param ring - ring buffer to get stats from
 * @param overflow_count - returns number of overflow events (dropped events)
 * @return 0 on success, negative on error
 */
int stlxgfx_event_ring_get_stats(const stlxgfx_event_ring_t* ring, uint32_t* overflow_count);

/**
 * Reset ring buffer statistics (overflow count)
 * @param ring - ring buffer to reset stats for
 * @return 0 on success, negative on error
 */
int stlxgfx_event_ring_reset_stats(stlxgfx_event_ring_t* ring);

/**
 * Get the size of the ring buffer structure
 * @return size in bytes of the ring buffer structure
 */
size_t stlxgfx_event_ring_get_size(void);

#endif // STLXGFX_EVENT_RING_H
