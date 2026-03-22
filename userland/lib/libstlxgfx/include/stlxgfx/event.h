#ifndef STLXGFX_EVENT_H
#define STLXGFX_EVENT_H

#include <stdint.h>
#include <stdatomic.h>

/* --- Window event types --- */

#define STLXGFX_EVT_KEY_DOWN          1
#define STLXGFX_EVT_KEY_UP            2
#define STLXGFX_EVT_POINTER_MOVE      3
#define STLXGFX_EVT_POINTER_BTN_DOWN  4
#define STLXGFX_EVT_POINTER_BTN_UP    5
#define STLXGFX_EVT_POINTER_SCROLL    6
#define STLXGFX_EVT_FOCUS_IN          7
#define STLXGFX_EVT_FOCUS_OUT         8

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

/* --- Client API --- */

struct stlxgfx_window_t_tag;
typedef struct stlxgfx_window_t_tag stlxgfx_window_t;

int stlxgfx_window_next_event(stlxgfx_window_t* window,
                               stlxgfx_event_t* event);

#endif /* STLXGFX_EVENT_H */
