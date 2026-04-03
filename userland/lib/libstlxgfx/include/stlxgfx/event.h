#ifndef STLXGFX_EVENT_H
#define STLXGFX_EVENT_H

#include <stdint.h>
#include <stdatomic.h>

/* --- Window event types --- */

#define STLXGFX_EVT_KEY_DOWN          0x0001
#define STLXGFX_EVT_KEY_UP            0x0002
#define STLXGFX_EVT_POINTER_MOVE      0x0010
#define STLXGFX_EVT_POINTER_BTN_DOWN  0x0011
#define STLXGFX_EVT_POINTER_BTN_UP    0x0012
#define STLXGFX_EVT_POINTER_SCROLL    0x0013
#define STLXGFX_EVT_FOCUS_IN          0x0020
#define STLXGFX_EVT_FOCUS_OUT         0x0021
#define STLXGFX_EVT_CLOSE_REQUESTED   0x0040
#define STLXGFX_EVT_RESIZE            0x0050

/* --- Window event record --- */

typedef struct {
    uint32_t type;
    uint32_t window_id;
    union {
        struct {
            uint16_t usage;
            uint8_t  modifiers;
            uint8_t  reserved;
        } key;
        struct {
            int32_t x;
            int32_t y;
            int16_t wheel;
            uint16_t buttons;
        } pointer;
        struct {
            uint32_t new_width;
            uint32_t new_height;
        } resize;
    };
} stlxgfx_event_t;

/* --- Shared-memory SPSC event ring --- */

#define STLXGFX_EVENT_RING_CAPACITY 64

typedef struct {
    atomic_uint_least32_t write_index;
    atomic_uint_least32_t read_index;
    uint32_t              overflow_count;
    uint32_t              reserved;
    stlxgfx_event_t       events[STLXGFX_EVENT_RING_CAPACITY];
} stlxgfx_event_ring_t;

/* --- Ring operations (usable by both DM and client) --- */

void stlxgfx_event_ring_init(stlxgfx_event_ring_t* ring);
int  stlxgfx_event_ring_write(stlxgfx_event_ring_t* ring,
                               const stlxgfx_event_t* event);
int  stlxgfx_event_ring_read(stlxgfx_event_ring_t* ring,
                              stlxgfx_event_t* event);

#endif /* STLXGFX_EVENT_H */
