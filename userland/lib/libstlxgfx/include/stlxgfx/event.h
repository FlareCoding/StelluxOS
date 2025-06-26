#ifndef STLXGFX_EVENT_H
#define STLXGFX_EVENT_H

#include <stdint.h>
#include "stlxgfx/window.h"
#include "stlxgfx/internal/stlxgfx_event_types.h"

/**
 * Event callback function type for applications
 * @param window - the window that received the event
 * @param event - the input event (keyboard, mouse, etc.)
 */
typedef void (*stlxgfx_event_callback_t)(stlxgfx_window_t* window, const stlxgfx_event_t* event);

/**
 * Set the global event callback function for the application
 * @param callback - function to call when events are received (can be NULL to disable)
 * @return 0 on success, negative on error
 */
int stlxgfx_set_event_callback(stlxgfx_event_callback_t callback);

/**
 * Poll for events from all windows and call the registered callback if events are available
 * This function checks if there are events to process and calls the callback function
 * if one is registered. If no callback is registered, events are simply ignored.
 * @return number of events processed, 0 if no events available, negative on error
 */
int stlxgfx_poll_events(void);

#endif // STLXGFX_EVENT_H
