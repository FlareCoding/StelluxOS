#include <stlxgfx/event.h>
#include <string.h>

void stlxgfx_event_ring_init(stlxgfx_event_ring_t* ring) {
    if (!ring) {
        return;
    }
    atomic_store_explicit(&ring->write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->read_index, 0, memory_order_relaxed);
    ring->overflow_count = 0;
    ring->reserved = 0;
    memset(ring->events, 0, sizeof(ring->events));
}

int stlxgfx_event_ring_write(stlxgfx_event_ring_t* ring,
                              const stlxgfx_event_t* event) {
    if (!ring || !event) {
        return -1;
    }

    uint32_t wi = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
    uint32_t next = (wi + 1) % STLXGFX_EVENT_RING_CAPACITY;
    uint32_t ri = atomic_load_explicit(&ring->read_index, memory_order_acquire);

    if (next == ri) {
        ring->overflow_count++;
        return -1;
    }

    ring->events[wi] = *event;
    atomic_store_explicit(&ring->write_index, next, memory_order_release);
    return 0;
}

int stlxgfx_event_ring_read(stlxgfx_event_ring_t* ring,
                             stlxgfx_event_t* event) {
    if (!ring || !event) {
        return -1;
    }

    uint32_t ri = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
    uint32_t wi = atomic_load_explicit(&ring->write_index, memory_order_acquire);

    if (ri == wi) {
        return 0;
    }

    *event = ring->events[ri];
    uint32_t next = (ri + 1) % STLXGFX_EVENT_RING_CAPACITY;
    atomic_store_explicit(&ring->read_index, next, memory_order_release);
    return 1;
}
