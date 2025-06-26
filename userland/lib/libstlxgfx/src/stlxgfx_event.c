#include <stdio.h>
#include <stdlib.h>
#include "stlxgfx/event.h"
#include "stlxgfx/internal/stlxgfx_event_ring.h"
#include "stlxgfx/internal/stlxgfx_event_types.h"
#include "stlxgfx/internal/stlxgfx_ctx.h"

// Global event callback function (single callback per application)
static stlxgfx_event_callback_t g_event_callback = NULL;

// Global list of windows with event ring buffers
// For now, we'll support a single window per application
static stlxgfx_window_t* g_current_window = NULL;

int stlxgfx_set_event_callback(stlxgfx_event_callback_t callback) {
    g_event_callback = callback;
    return 0;
}

int stlxgfx_poll_events(void) {
    int total_events_processed = 0;
    
    // If no callback is registered, just check for events but don't process them
    if (!g_event_callback) {
        // Still check if there are events to avoid buffer overflow
        if (g_current_window && g_current_window->event_ring) {
            int available = stlxgfx_event_ring_available_read(g_current_window->event_ring);
            if (available > 0) {
                // Events are available but no callback - just return 0 to indicate no processing
                return 0;
            }
        }
        return 0;
    }
    
    // Process events from the current window
    if (g_current_window && g_current_window->event_ring) {
        stlxgfx_event_t event;
        int events_processed = 0;
        
        // Read all available events from the ring buffer
        while (stlxgfx_event_ring_read(g_current_window->event_ring, &event) == 0) {
            // Call the registered callback function
            g_event_callback(g_current_window, &event);
            events_processed++;
            total_events_processed++;
        }
        
        if (events_processed > 0) {
            // Optional: log event processing for debugging
            // printf("STLXGFX: Processed %d events from window %u\n", 
            //        events_processed, g_current_window->window_id);
        }
    }
    
    return total_events_processed;
}

// Internal function to register a window for event processing
// This is called by the window creation function
int stlxgfx_register_window_for_events(stlxgfx_window_t* window) {
    if (!window || !window->initialized || !window->event_ring) {
        return -1;
    }
    
    // For now, we only support one window per application
    if (g_current_window != NULL) {
        printf("STLXGFX: Warning - replacing existing window for event processing\n");
    }
    
    g_current_window = window;
    return 0;
}

// Internal function to unregister a window from event processing
// This is called by the window destruction function
int stlxgfx_unregister_window_from_events(stlxgfx_window_t* window) {
    if (!window) {
        return -1;
    }
    
    if (g_current_window == window) {
        g_current_window = NULL;
    }
    
    return 0;
}
