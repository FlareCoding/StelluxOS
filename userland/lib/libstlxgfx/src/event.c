#include <stlxgfx/event.h>
#include <stlxgfx/window.h>

int stlxgfx_window_next_event(stlxgfx_window_t* window,
                               stlxgfx_event_t* event) {
    if (!window || !event || !window->event_ring) {
        return -1;
    }
    return stlxgfx_event_ring_read(window->event_ring, event);
}
