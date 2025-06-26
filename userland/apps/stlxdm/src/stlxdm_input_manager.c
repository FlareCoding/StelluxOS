#define _POSIX_C_SOURCE 199309L
#include "stlxdm_input_manager.h"
#include "stlxdm_compositor.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stlxgfx/internal/stlxgfx_event_ring.h>

// Input grab types
#define STLXDM_GRAB_KEYBOARD    0x01
#define STLXDM_GRAB_MOUSE       0x02
#define STLXDM_GRAB_BOTH        (STLXDM_GRAB_KEYBOARD | STLXDM_GRAB_MOUSE)

// Forward declarations for internal functions
static int _handle_keyboard_event(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event);
static int _handle_mouse_event(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event);
static void _update_modifier_state(stlxdm_input_manager_t* input_mgr, uint32_t keycode, bool pressed);
static stlxdm_global_shortcut_t _check_global_shortcuts(stlxdm_input_manager_t* input_mgr, uint32_t keycode);
static int _route_event_to_focused_window(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event);
static uint32_t _get_current_time_ms(void);
static int _initiate_window_drag(stlxdm_input_manager_t* input_mgr, stlxdm_client_info_t* client, int32_t click_x, int32_t click_y);
static int _terminate_window_drag(stlxdm_input_manager_t* input_mgr);

int stlxdm_input_manager_init(stlxdm_input_manager_t* input_mgr, 
                             stlxdm_compositor_t* compositor,
                             stlxdm_server_t* server) {
    if (!input_mgr || !compositor || !server) {
        STLXDM_INPUT_TRACE("ERROR: Invalid parameters for input manager init");
        return -1;
    }
    
    // Clear the entire structure
    memset(input_mgr, 0, sizeof(stlxdm_input_manager_t));
    
    // Set component references
    input_mgr->server = server;
    
    // Initialize cursor state
    input_mgr->cursor_x = STLXDM_INPUT_CURSOR_DEFAULT_X;
    input_mgr->cursor_y = STLXDM_INPUT_CURSOR_DEFAULT_Y;
    
    // Get screen boundaries from compositor
    const struct gfx_framebuffer_info* fb_info = stlxdm_compositor_get_fb_info(compositor);
    if (fb_info) {
        input_mgr->cursor_max_x = fb_info->width - 1;
        input_mgr->cursor_max_y = fb_info->height - 1;
    } else {
        // Fallback to reasonable defaults
        input_mgr->cursor_max_x = 1023;
        input_mgr->cursor_max_y = 767;
    }
    
    input_mgr->cursor_visible = true;
    input_mgr->cursor_needs_redraw = true;
    
    // Initialize focus state
    input_mgr->focused_window_id = 0;
    input_mgr->focused_client = NULL;
    input_mgr->last_click_window_id = 0;
    
    // Initialize configuration with sensible defaults
    input_mgr->config.enable_focus_follows_mouse = false;  // Click to focus by default
    input_mgr->config.enable_click_to_focus = true;
    input_mgr->config.enable_global_shortcuts = true;
    input_mgr->config.enable_cursor_acceleration = false;
    input_mgr->config.double_click_timeout_ms = 300;
    
    // Initialize input grab state
    input_mgr->input_grabbed = false;
    input_mgr->grab_window_id = 0;
    input_mgr->grab_type = 0;
    
    // Initialize drag state
    input_mgr->drag_state.is_dragging = false;
    input_mgr->drag_state.drag_type = STLXDM_DRAG_TYPE_NONE;
    input_mgr->drag_state.dragged_window_id = 0;
    input_mgr->drag_state.dragged_client = NULL;
    input_mgr->drag_state.drag_start_x = 0;
    input_mgr->drag_state.drag_start_y = 0;
    input_mgr->drag_state.window_start_x = 0;
    input_mgr->drag_state.window_start_y = 0;
    input_mgr->drag_state.drag_offset_x = 0;
    input_mgr->drag_state.drag_offset_y = 0;
    input_mgr->drag_state.drag_start_time_ms = 0;
    
    // Initialize timing state
    input_mgr->last_click_time_ms = 0;
    input_mgr->last_clicked_button = 0;
    
    input_mgr->initialized = 1;
    
    STLXDM_INPUT_TRACE("Input manager initialized (cursor bounds: %dx%d)", 
           input_mgr->cursor_max_x + 1, input_mgr->cursor_max_y + 1);
    
    return 0;
}

void stlxdm_input_manager_cleanup(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return;
    }
    
    STLXDM_INPUT_TRACE("Input manager cleanup (processed %lu events total)", 
           input_mgr->stats.total_events_processed);
    
    // Clear all state
    memset(input_mgr, 0, sizeof(stlxdm_input_manager_t));
}

int stlxdm_input_manager_process_events(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    struct input_event_t events[STLXDM_INPUT_MAX_EVENTS_PER_FRAME];
    long events_read = stlx_read_input_events(INPUT_QUEUE_ID_SYSTEM, 0, events, STLXDM_INPUT_MAX_EVENTS_PER_FRAME);
    
    if (events_read < 0) {
        STLXDM_INPUT_TRACE("Error reading input events: %ld", events_read);
        return -1;
    }
    
    if (events_read == 0) {
        // No events available
        input_mgr->stats.events_this_frame = 0;
        return 0;
    }
    
    input_mgr->stats.events_this_frame = events_read;
    input_mgr->stats.total_events_processed += events_read;
    
    // Process each event
    for (int i = 0; i < events_read; i++) {
        const struct input_event_t* event = &events[i];
        
        switch (event->type) {
            case KBD_EVT_KEY_PRESSED:
            case KBD_EVT_KEY_RELEASED:
                input_mgr->stats.keyboard_events++;
                _handle_keyboard_event(input_mgr, event);
                break;
                
            case POINTER_EVT_MOUSE_MOVED:
            case POINTER_EVT_MOUSE_BTN_PRESSED:
            case POINTER_EVT_MOUSE_BTN_RELEASED:
            case POINTER_EVT_MOUSE_SCROLLED:
                input_mgr->stats.mouse_events++;
                _handle_mouse_event(input_mgr, event);
                break;
                
            default:
                STLXDM_INPUT_TRACE("Unknown input event type: %u", event->type);
                break;
        }
    }
    
    return events_read;
}

static int _handle_keyboard_event(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event) {
    uint32_t keycode = event->udata1;
    uint32_t modifiers __attribute__((unused)) = event->udata2;
    bool is_pressed = (event->type == KBD_EVT_KEY_PRESSED);
    
    // Update modifier key state
    _update_modifier_state(input_mgr, keycode, is_pressed);
    
    // Check for global shortcuts on key press
    if (is_pressed && input_mgr->config.enable_global_shortcuts) {
        stlxdm_global_shortcut_t shortcut = _check_global_shortcuts(input_mgr, keycode);
        if (shortcut != STLXDM_SHORTCUT_NONE) {
            input_mgr->stats.global_shortcuts_triggered++;
            
            switch (shortcut) {
                case STLXDM_SHORTCUT_CTRL_ALT_ESC:
                    STLXDM_INPUT_TRACE("Global shortcut: Force quit requested");
                    return 0;
                    
                case STLXDM_SHORTCUT_ALT_TAB:
                    STLXDM_INPUT_TRACE("Global shortcut: Window switcher (not implemented)");
                    return 0;
                    
                case STLXDM_SHORTCUT_CTRL_ALT_T:
                    STLXDM_INPUT_TRACE("Global shortcut: Terminal (not implemented)");
                    return 0;
                    
                default:
                    break;
            }
            
            // Don't route global shortcuts to applications
            return 0;
        }
    }
    
    // Route to focused window if not grabbed or if this window has grab
    if (!input_mgr->input_grabbed || 
        (input_mgr->grab_type & STLXDM_GRAB_KEYBOARD)) {
        _route_event_to_focused_window(input_mgr, event);
    }
    
    return 0;
}

static int _handle_mouse_event(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event) {
    switch (event->type) {
        case POINTER_EVT_MOUSE_MOVED: {
            // Update cursor position
            int32_t new_x = event->udata1;
            int32_t new_y = event->udata2;
            
            // Clamp to screen boundaries
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if (new_x > input_mgr->cursor_max_x) new_x = input_mgr->cursor_max_x;
            if (new_y > input_mgr->cursor_max_y) new_y = input_mgr->cursor_max_y;
            
            // Check if cursor actually moved
            if (new_x != input_mgr->cursor_x || new_y != input_mgr->cursor_y) {
                input_mgr->cursor_x = new_x;
                input_mgr->cursor_y = new_y;
                input_mgr->cursor_needs_redraw = true;
                
                // Handle window dragging if active
                if (input_mgr->drag_state.is_dragging && 
                    input_mgr->drag_state.drag_type == STLXDM_DRAG_TYPE_MOVE &&
                    input_mgr->drag_state.dragged_client && 
                    input_mgr->drag_state.dragged_client->window) {
                    
                    // Calculate new window position based on cursor movement and drag offset
                    int32_t new_window_x = new_x - input_mgr->drag_state.drag_offset_x;
                    int32_t new_window_y = new_y - input_mgr->drag_state.drag_offset_y;
                    
                    // Apply boundary constraints
                    if (new_window_x < -STLXDM_DRAG_BOUNDARY_MARGIN) {
                        new_window_x = -STLXDM_DRAG_BOUNDARY_MARGIN;
                    }
                    if (new_window_y < -STLXDM_DRAG_BOUNDARY_MARGIN) {
                        new_window_y = -STLXDM_DRAG_BOUNDARY_MARGIN;
                    }
                    if (new_window_x + (int32_t)input_mgr->drag_state.dragged_client->window->width > 
                        input_mgr->cursor_max_x + STLXDM_DRAG_BOUNDARY_MARGIN) {
                        new_window_x = input_mgr->cursor_max_x + STLXDM_DRAG_BOUNDARY_MARGIN - 
                                      (int32_t)input_mgr->drag_state.dragged_client->window->width;
                    }
                    if (new_window_y + (int32_t)input_mgr->drag_state.dragged_client->window->height > 
                        input_mgr->cursor_max_y + STLXDM_DRAG_BOUNDARY_MARGIN) {
                        new_window_y = input_mgr->cursor_max_y + STLXDM_DRAG_BOUNDARY_MARGIN - 
                                      (int32_t)input_mgr->drag_state.dragged_client->window->height;
                    }
                    
                    // Update window position
                    input_mgr->drag_state.dragged_client->window->posx = new_window_x;
                    input_mgr->drag_state.dragged_client->window->posy = new_window_y;
                }
                
                // Focus follows mouse if enabled
                if (input_mgr->config.enable_focus_follows_mouse) {
                    stlxdm_client_info_t* window_under_cursor = 
                        stlxdm_input_manager_find_window_at_position(input_mgr, new_x, new_y);
                    if (window_under_cursor && window_under_cursor != input_mgr->focused_client) {
                        stlxdm_input_manager_set_focus(input_mgr, window_under_cursor);
                    }
                }
                
                // Route mouse movement to focused window if cursor is over it
                if (input_mgr->focused_client && input_mgr->focused_client->window) {
                    stlxdm_client_info_t* window_under_cursor = 
                        stlxdm_input_manager_find_window_at_position(input_mgr, new_x, new_y);
                    
                    // Only route if cursor is over the focused window
                    if (window_under_cursor == input_mgr->focused_client) {
                        _route_event_to_focused_window(input_mgr, event);
                    }
                }
            }
            break;
        }
        
        case POINTER_EVT_MOUSE_BTN_PRESSED: {
            uint32_t button = event->udata1;
            uint32_t current_time = _get_current_time_ms();
            
            // Find window under cursor and focus it immediately on press
            stlxdm_client_info_t* clicked_window = 
                stlxdm_input_manager_find_window_at_position(input_mgr, 
                                                           input_mgr->cursor_x, 
                                                           input_mgr->cursor_y);
            
            if (clicked_window && clicked_window->window) {
                // Focus the window immediately on button press
                if (clicked_window != input_mgr->focused_client) {
                    stlxdm_input_manager_set_focus(input_mgr, clicked_window);
                }
                
                // Check for drag initiation (left mouse button on title bar)
                if (button == 1) { // Left mouse button
                    window_region_t region = stlxdm_hit_test_window(clicked_window->window, 
                                                                   input_mgr->cursor_x, 
                                                                   input_mgr->cursor_y);
                    
                    if (region == WINDOW_REGION_TITLE_BAR) {
                        // Initiate window drag
                        if (_initiate_window_drag(input_mgr, clicked_window, 
                                                 input_mgr->cursor_x, input_mgr->cursor_y) == 0) {
                            // STLXDM_INPUT_TRACE("Drag initiated for window %u", 
                            //        clicked_window->window->window_id);
                        }
                    } else if (region == WINDOW_REGION_CLIENT_AREA) {
                        // Route button press to client area
                        _route_event_to_focused_window(input_mgr, event);
                    }
                }
            } else {
                // Clicked outside any window - clear focus
                if (input_mgr->focused_client) {
                    // STLXDM_INPUT_TRACE("Clicked outside window - clearing focus");
                    stlxdm_input_manager_set_focus(input_mgr, NULL);
                }
            }
            
            // Double-click detection
            bool is_double_click = false;
            if (button == input_mgr->last_clicked_button &&
                (current_time - input_mgr->last_click_time_ms) < input_mgr->config.double_click_timeout_ms) {
                is_double_click = true;
                __unused is_double_click;
                // STLXDM_INPUT_TRACE("Double-click detected (button %u)", button);
            }
            
            input_mgr->last_clicked_button = button;
            input_mgr->last_click_time_ms = current_time;
            break;
        }
        
        case POINTER_EVT_MOUSE_BTN_RELEASED: {
            uint32_t button = event->udata1;
            
            // Handle drag termination (left mouse button release)
            if (button == 1 && input_mgr->drag_state.is_dragging) {
                // STLXDM_INPUT_TRACE("Drag terminated for window %u", 
                //        input_mgr->drag_state.dragged_window_id);
                _terminate_window_drag(input_mgr);
                break; // Skip normal click handling when dragging
            }
            
            // Find window under cursor for click actions
            stlxdm_client_info_t* clicked_window = 
                stlxdm_input_manager_find_window_at_position(input_mgr, 
                                                           input_mgr->cursor_x, 
                                                           input_mgr->cursor_y);
            
            if (clicked_window && clicked_window->window) {
                // Hit test the window to determine which region was clicked
                window_region_t region = stlxdm_hit_test_window(clicked_window->window, 
                                                               input_mgr->cursor_x, 
                                                               input_mgr->cursor_y);
                
                // Debug: Show window bounds
                // STLXDM_INPUT_TRACE("DEBUG: Window %u at (%d,%d) size %dx%d, click at (%d,%d)",
                //        clicked_window->window->window_id,
                //        clicked_window->window->posx, clicked_window->window->posy,
                //        clicked_window->window->width, clicked_window->window->height,
                //        input_mgr->cursor_x, input_mgr->cursor_y);
                
                // Handle different regions (click actions happen on release)
                switch (region) {
                    case WINDOW_REGION_CLOSE_BUTTON:
                        STLXDM_INPUT_TRACE("Close button clicked for window %u (stub - would close window)", 
                               clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_TITLE_BAR:
                        // STLXDM_INPUT_TRACE("Title bar clicked for window %u", 
                        //        clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_BORDER:
                        // STLXDM_INPUT_TRACE("Border clicked for window %u", 
                        //        clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_CLIENT_AREA: {
                        // Calculate client-relative coordinates
                        int32_t rel_x = input_mgr->cursor_x - clicked_window->window->posx;
                        int32_t rel_y = input_mgr->cursor_y - clicked_window->window->posy;
                        __unused rel_x;
                        __unused rel_y;
                        
                        // Route client area events to the application
                        _route_event_to_focused_window(input_mgr, event);
                        break;
                    }
                    
                    case WINDOW_REGION_NONE:
                    default:
                        STLXDM_INPUT_TRACE("Unknown region clicked for window %u", 
                               clicked_window->window->window_id);
                        break;
                }
            }

            break;
        }
        
        case POINTER_EVT_MOUSE_SCROLLED: {
            // Route scroll events to focused window if cursor is over it
            if (input_mgr->focused_client && input_mgr->focused_client->window) {
                stlxdm_client_info_t* window_under_cursor = 
                    stlxdm_input_manager_find_window_at_position(input_mgr, 
                                                               input_mgr->cursor_x, 
                                                               input_mgr->cursor_y);
                
                // Only route if cursor is over the focused window
                if (window_under_cursor == input_mgr->focused_client) {
                    _route_event_to_focused_window(input_mgr, event);
                }
            }
            break;
        }
        default:
            // Not a mouse event - should not reach here
            return -1;
    }
    
    return 0;
}

static void _update_modifier_state(stlxdm_input_manager_t* input_mgr, uint32_t keycode, bool pressed) {
    // Update modifier state based on keycode
    switch (keycode) {
        case 0xE0: // Left Control
            input_mgr->modifiers.ctrl_left = pressed;
            break;
        case 0xE4: // Right Control  
            input_mgr->modifiers.ctrl_right = pressed;
            break;
        case 0xE2: // Left Alt
            input_mgr->modifiers.alt_left = pressed;
            break;
        case 0xE6: // Right Alt
            input_mgr->modifiers.alt_right = pressed;
            break;
        case 0xE1: // Left Shift
            input_mgr->modifiers.shift_left = pressed;
            break;
        case 0xE5: // Right Shift
            input_mgr->modifiers.shift_right = pressed;
            break;
        case 0xE3: // Left Super/Windows
            input_mgr->modifiers.super_left = pressed;
            break;
        case 0xE7: // Right Super/Windows
            input_mgr->modifiers.super_right = pressed;
            break;
    }
}

static stlxdm_global_shortcut_t _check_global_shortcuts(stlxdm_input_manager_t* input_mgr, uint32_t keycode) {
    bool ctrl = input_mgr->modifiers.ctrl_left || input_mgr->modifiers.ctrl_right;
    bool alt = input_mgr->modifiers.alt_left || input_mgr->modifiers.alt_right;
    bool shift = input_mgr->modifiers.shift_left || input_mgr->modifiers.shift_right;

    // Check various global shortcut combinations
    if (ctrl && alt) {
        if (keycode == 0x29) { // Escape key
            return STLXDM_SHORTCUT_CTRL_ALT_ESC;
        }
        if (keycode == 0x17) { // T key
            return STLXDM_SHORTCUT_CTRL_ALT_T;
        }
    }

    if (alt && !ctrl && !shift) {
        if (keycode == 0x2B) { // Tab key
            return STLXDM_SHORTCUT_ALT_TAB;
        }
    }

    return STLXDM_SHORTCUT_NONE;
}

static int _route_event_to_focused_window(stlxdm_input_manager_t* input_mgr, const struct input_event_t* event) {
    if (!input_mgr->focused_client || !input_mgr->focused_client->window) {
        // No focused window - route to system console
        if (event->type == KBD_EVT_KEY_PRESSED) {
            printf("%c", event->sdata1);  // Simple fallback
            fflush(stdout);
        }
        return 0;
    }

    // Get the focused window's event ring
    stlxgfx_window_t* window = input_mgr->focused_client->window;
    if (!window->event_ring) {
        STLXDM_INPUT_TRACE("ERROR: No event ring for focused window %u", window->window_id);
        return -1;
    }

    // Convert kernel event to userland event format (they're compatible)
    stlxgfx_event_t userland_event;
    userland_event.id = event->id;
    userland_event.type = (stlxgfx_input_event_type_t)event->type;  // Cast is safe since types match
    userland_event.udata1 = event->udata1;
    userland_event.udata2 = event->udata2;
    userland_event.sdata1 = event->sdata1;
    userland_event.sdata2 = event->sdata2;

    // Write event to the window's event ring
    int write_result = stlxgfx_event_ring_write(window->event_ring, &userland_event);
    if (write_result != 0) {
        // Event ring is full - this is normal for high-frequency events like mouse movement
        if (event->type != POINTER_EVT_MOUSE_MOVED) {
            STLXDM_INPUT_TRACE("WARNING: Event ring full for window %u, dropping event type %u", 
               window->window_id, event->type);
        }
        return -1;
    }

    return 0;
}

int stlxdm_input_manager_set_focus(stlxdm_input_manager_t* input_mgr, stlxdm_client_info_t* client) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }

    stlxdm_client_info_t* old_client = input_mgr->focused_client;
    uint32_t old_window_id = input_mgr->focused_window_id;
    __unused old_window_id;

    if (client) {
        input_mgr->focused_client = client;
        input_mgr->focused_window_id = client->window ? client->window->window_id : 0;
    } else {
        input_mgr->focused_client = NULL;
        input_mgr->focused_window_id = 0;
    }

    if (old_client != client) {
        input_mgr->stats.focus_changes++;
        // STLXDM_INPUT_TRACE("Focus changed: %u -> %u", old_window_id, input_mgr->focused_window_id);
    }

    return 0;
}

int stlxdm_input_manager_get_cursor_position(const stlxdm_input_manager_t* input_mgr, int32_t* x, int32_t* y) {
    if (!input_mgr || !input_mgr->initialized || !x || !y) {
        return -1;
    }
    
    *x = input_mgr->cursor_x;
    *y = input_mgr->cursor_y;
    return 0;
}

int stlxdm_input_manager_set_cursor_position(stlxdm_input_manager_t* input_mgr, int32_t x, int32_t y) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    // Clamp to screen boundaries
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > input_mgr->cursor_max_x) x = input_mgr->cursor_max_x;
    if (y > input_mgr->cursor_max_y) y = input_mgr->cursor_max_y;
    
    if (x != input_mgr->cursor_x || y != input_mgr->cursor_y) {
        input_mgr->cursor_x = x;
        input_mgr->cursor_y = y;
        input_mgr->cursor_needs_redraw = true;
    }
    
    return 0;
}

uint32_t stlxdm_input_manager_get_focused_window_id(const stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return 0;
    }
    
    return input_mgr->focused_window_id;
}

stlxdm_client_info_t* stlxdm_input_manager_find_window_at_position(
    const stlxdm_input_manager_t* input_mgr, int32_t x, int32_t y) {
    
    if (!input_mgr || !input_mgr->initialized || !input_mgr->server) {
        return NULL;
    }
    
    // Check all clients for window bounds (including decorations)
    for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
        const stlxdm_client_info_t* client = &input_mgr->server->clients[i];
        
        if (client->state != STLXDM_CLIENT_CONNECTED || !client->window) {
            continue;
        }
        
        // Calculate full window bounds including decorations
        int32_t window_x = client->window->posx - WINDOW_BORDER_WIDTH;
        int32_t window_y = client->window->posy - WINDOW_TITLE_BAR_HEIGHT - WINDOW_BORDER_WIDTH;
        int32_t window_width = client->window->width + (2 * WINDOW_BORDER_WIDTH);
        int32_t window_height = client->window->height + WINDOW_TITLE_BAR_HEIGHT + (2 * WINDOW_BORDER_WIDTH);
        
        if (x >= window_x && x < window_x + window_width &&
            y >= window_y && y < window_y + window_height) {
            return (stlxdm_client_info_t*)client;  // Cast away const for return
        }
    }
    
    return NULL;
}

bool stlxdm_input_manager_cursor_needs_redraw(const stlxdm_input_manager_t* input_mgr) {
    return input_mgr && input_mgr->initialized && input_mgr->cursor_needs_redraw;
}

void stlxdm_input_manager_mark_cursor_drawn(stlxdm_input_manager_t* input_mgr) {
    if (input_mgr && input_mgr->initialized) {
        input_mgr->cursor_needs_redraw = false;
    }
}

int stlxdm_input_manager_grab_input(stlxdm_input_manager_t* input_mgr, uint32_t window_id, uint32_t grab_type) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    input_mgr->input_grabbed = true;
    input_mgr->grab_window_id = window_id;
    input_mgr->grab_type = grab_type;
    
    STLXDM_INPUT_TRACE("Input grabbed by window %u (type: 0x%02x)", window_id, grab_type);
    return 0;
}

int stlxdm_input_manager_ungrab_input(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    STLXDM_INPUT_TRACE("Input ungrabbed (was window %u)", input_mgr->grab_window_id);
    
    input_mgr->input_grabbed = false;
    input_mgr->grab_window_id = 0;
    input_mgr->grab_type = 0;
    
    return 0;
}

const void* stlxdm_input_manager_get_stats(const stlxdm_input_manager_t* input_mgr) {
    return input_mgr && input_mgr->initialized ? &input_mgr->stats : NULL;
}

static uint32_t _get_current_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    }
    return 0;
}

static int _initiate_window_drag(stlxdm_input_manager_t* input_mgr, stlxdm_client_info_t* client, int32_t click_x, int32_t click_y) {
    if (!input_mgr || !client || !client->window) {
        return -1;
    }
    
    input_mgr->drag_state.is_dragging = true;
    input_mgr->drag_state.drag_type = STLXDM_DRAG_TYPE_MOVE;
    input_mgr->drag_state.dragged_window_id = client->window->window_id;
    input_mgr->drag_state.dragged_client = client;
    input_mgr->drag_state.drag_start_x = click_x;
    input_mgr->drag_state.drag_start_y = click_y;
    input_mgr->drag_state.window_start_x = client->window->posx;
    input_mgr->drag_state.window_start_y = client->window->posy;
    input_mgr->drag_state.drag_offset_x = click_x - client->window->posx;
    input_mgr->drag_state.drag_offset_y = click_y - client->window->posy;
    input_mgr->drag_state.drag_start_time_ms = _get_current_time_ms();
    
    return 0;
}

static int _terminate_window_drag(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    input_mgr->drag_state.is_dragging = false;
    input_mgr->drag_state.drag_type = STLXDM_DRAG_TYPE_NONE;
    input_mgr->drag_state.dragged_window_id = 0;
    input_mgr->drag_state.dragged_client = NULL;
    input_mgr->drag_state.drag_start_x = 0;
    input_mgr->drag_state.drag_start_y = 0;
    input_mgr->drag_state.window_start_x = 0;
    input_mgr->drag_state.window_start_y = 0;
    input_mgr->drag_state.drag_offset_x = 0;
    input_mgr->drag_state.drag_offset_y = 0;
    input_mgr->drag_state.drag_start_time_ms = 0;
    
    return 0;
}
 