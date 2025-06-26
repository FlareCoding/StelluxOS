#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "stlxgfx/internal/stlxgfx_event_ring.h"

int stlxgfx_event_ring_init(stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    // Initialize header fields
    atomic_init(&ring->header.read_index, 0);
    atomic_init(&ring->header.write_index, 0);
    ring->header.buffer_size = STLXGFX_EVENT_RING_CAPACITY;
    ring->header.event_size = sizeof(stlxgfx_event_t);
    ring->header.overflow_count = 0;
    
    // Clear reserved fields
    memset(ring->header.reserved, 0, sizeof(ring->header.reserved));
    
    // Clear all event slots
    memset(ring->events, 0, sizeof(ring->events));
    
    return 0;
}

int stlxgfx_event_ring_is_empty(const stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    
    return (read_idx == write_idx) ? 1 : 0;
}

int stlxgfx_event_ring_is_full(const stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    uint32_t next_write = (write_idx + 1) % STLXGFX_EVENT_RING_CAPACITY;
    
    return (next_write == read_idx) ? 1 : 0;
}

int stlxgfx_event_ring_available_read(const stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    
    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    } else {
        return STLXGFX_EVENT_RING_CAPACITY - read_idx + write_idx;
    }
}

int stlxgfx_event_ring_available_write(const stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    uint32_t next_write = (write_idx + 1) % STLXGFX_EVENT_RING_CAPACITY;
    
    if (next_write >= read_idx) {
        return STLXGFX_EVENT_RING_CAPACITY - next_write + read_idx;
    } else {
        return read_idx - next_write;
    }
}

int stlxgfx_event_ring_read(stlxgfx_event_ring_t* ring, stlxgfx_event_t* event) {
    if (!ring || !event) {
        return -1;
    }
    
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    
    // Check if buffer is empty
    if (read_idx == write_idx) {
        return -1; // Buffer empty
    }
    
    // Copy event data
    memcpy(event, &ring->events[read_idx], sizeof(stlxgfx_event_t));
    
    // Update read index
    uint32_t next_read = (read_idx + 1) % STLXGFX_EVENT_RING_CAPACITY;
    atomic_store_explicit(&ring->header.read_index, next_read, memory_order_release);
    
    return 0;
}

int stlxgfx_event_ring_write(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* event) {
    if (!ring || !event) {
        return -1;
    }
    
    uint32_t write_idx = atomic_load_explicit(&ring->header.write_index, memory_order_relaxed);
    uint32_t next_write = (write_idx + 1) % STLXGFX_EVENT_RING_CAPACITY;
    uint32_t read_idx = atomic_load_explicit(&ring->header.read_index, memory_order_acquire);
    
    // Check if buffer is full
    if (next_write == read_idx) {
        // Buffer full - increment overflow counter and drop event
        ring->header.overflow_count++;
        return -1; // Buffer full
    }
    
    // Copy event data
    memcpy(&ring->events[write_idx], event, sizeof(stlxgfx_event_t));
    
    // Update write index
    atomic_store_explicit(&ring->header.write_index, next_write, memory_order_release);
    
    return 0;
}

int stlxgfx_event_ring_read_batch(stlxgfx_event_ring_t* ring, stlxgfx_event_t* events, int max_events) {
    if (!ring || !events || max_events <= 0) {
        return -1;
    }
    
    int events_read = 0;
    
    for (int i = 0; i < max_events; i++) {
        if (stlxgfx_event_ring_read(ring, &events[i]) != 0) {
            break; // No more events to read
        }
        events_read++;
    }
    
    return events_read;
}

int stlxgfx_event_ring_write_batch(stlxgfx_event_ring_t* ring, const stlxgfx_event_t* events, int num_events) {
    if (!ring || !events || num_events <= 0) {
        return -1;
    }
    
    int events_written = 0;
    
    for (int i = 0; i < num_events; i++) {
        if (stlxgfx_event_ring_write(ring, &events[i]) != 0) {
            break; // Buffer full
        }
        events_written++;
    }
    
    return events_written;
}

int stlxgfx_event_ring_get_stats(const stlxgfx_event_ring_t* ring, uint32_t* overflow_count) {
    if (!ring || !overflow_count) {
        return -1;
    }
    
    *overflow_count = ring->header.overflow_count;
    return 0;
}

int stlxgfx_event_ring_reset_stats(stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return -1;
    }
    
    ring->header.overflow_count = 0;
    return 0;
}

size_t stlxgfx_event_ring_get_size(void) {
    return sizeof(stlxgfx_event_ring_t);
}
