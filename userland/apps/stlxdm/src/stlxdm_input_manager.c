#define _POSIX_C_SOURCE 199309L
#include "stlxdm_input_manager.h"
#include "stlxdm_compositor.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

int stlxdm_input_manager_init(stlxdm_input_manager_t* input_mgr, 
                             stlxdm_compositor_t* compositor,
                             stlxdm_server_t* server) {
    if (!input_mgr || !compositor || !server) {
        printf("[STLXDM_INPUT] ERROR: Invalid parameters for input manager init\n");
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
    
    // Initialize timing state
    input_mgr->last_click_time_ms = 0;
    input_mgr->last_clicked_button = 0;
    
    input_mgr->initialized = 1;
    
    printf("[STLXDM_INPUT] Input manager initialized (cursor bounds: %dx%d)\n", 
           input_mgr->cursor_max_x + 1, input_mgr->cursor_max_y + 1);
    
    return 0;
}

void stlxdm_input_manager_cleanup(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return;
    }
    
    printf("[STLXDM_INPUT] Input manager cleanup (processed %lu events total)\n", 
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
        printf("[STLXDM_INPUT] Error reading input events: %ld\n", events_read);
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
                printf("[STLXDM_INPUT] Unknown input event type: %u\n", event->type);
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
                    printf("[STLXDM_INPUT] Global shortcut: Force quit requested\n");
                    return 0;
                    
                case STLXDM_SHORTCUT_ALT_TAB:
                    printf("[STLXDM_INPUT] Global shortcut: Window switcher (not implemented)\n");
                    return 0;
                    
                case STLXDM_SHORTCUT_CTRL_ALT_T:
                    printf("[STLXDM_INPUT] Global shortcut: Terminal (not implemented)\n");
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
                
                // Focus follows mouse if enabled
                if (input_mgr->config.enable_focus_follows_mouse) {
                    stlxdm_client_info_t* window_under_cursor = 
                        stlxdm_input_manager_find_window_at_position(input_mgr, new_x, new_y);
                    if (window_under_cursor && window_under_cursor != input_mgr->focused_client) {
                        stlxdm_input_manager_set_focus(input_mgr, window_under_cursor);
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
            } else {
                // Clicked outside any window - clear focus
                if (input_mgr->focused_client) {
                    printf("[STLXDM_INPUT] Clicked outside window - clearing focus\n");
                    stlxdm_input_manager_set_focus(input_mgr, NULL);
                }
            }
            
            // Double-click detection
            bool is_double_click = false;
            if (button == input_mgr->last_clicked_button &&
                (current_time - input_mgr->last_click_time_ms) < input_mgr->config.double_click_timeout_ms) {
                is_double_click = true;
                __unused is_double_click;
                printf("[STLXDM_INPUT] Double-click detected (button %u)\n", button);
            }
            
            input_mgr->last_clicked_button = button;
            input_mgr->last_click_time_ms = current_time;
            break;
        }
        
        case POINTER_EVT_MOUSE_BTN_RELEASED: {
            uint32_t button = event->udata1;
            
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
                printf("[STLXDM_INPUT] DEBUG: Window %u at (%d,%d) size %dx%d, click at (%d,%d)\n",
                       clicked_window->window->window_id,
                       clicked_window->window->posx, clicked_window->window->posy,
                       clicked_window->window->width, clicked_window->window->height,
                       input_mgr->cursor_x, input_mgr->cursor_y);
                
                // Handle different regions (click actions happen on release)
                switch (region) {
                    case WINDOW_REGION_CLOSE_BUTTON:
                        printf("[STLXDM_INPUT] Close button clicked for window %u (stub - would close window)\n", 
                               clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_TITLE_BAR:
                        printf("[STLXDM_INPUT] Title bar clicked for window %u\n", 
                               clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_BORDER:
                        printf("[STLXDM_INPUT] Border clicked for window %u\n", 
                               clicked_window->window->window_id);
                        break;
                        
                    case WINDOW_REGION_CLIENT_AREA: {
                        // Calculate client-relative coordinates
                        int32_t rel_x = input_mgr->cursor_x - clicked_window->window->posx;
                        int32_t rel_y = input_mgr->cursor_y - clicked_window->window->posy;
                        
                        printf("[STLXDM_INPUT] Client area clicked for window %u - would propagate event to app (coords: %d, %d)\n", 
                               clicked_window->window->window_id, rel_x, rel_y);
                        
                        // Route client area events to the application
                        _route_event_to_focused_window(input_mgr, event);
                        break;
                    }
                    
                    case WINDOW_REGION_NONE:
                    default:
                        printf("[STLXDM_INPUT] Unknown region clicked for window %u\n", 
                               clicked_window->window->window_id);
                        break;
                }
            }
            
            printf("[STLXDM_INPUT] Mouse button %u released at (%d,%d)\n", 
                   button, input_mgr->cursor_x, input_mgr->cursor_y);
            break;
        }
        
        case POINTER_EVT_MOUSE_SCROLLED: {
            // int32_t scroll_delta = event->sdata2;
            // const char* direction = (event->udata1 == 0) ? "vertical" : "horizontal";
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

    // Here you would send the event to the focused window via IPC
    // For now, just log it
    if (event->type != POINTER_EVT_MOUSE_MOVED) {
        printf("[STLXDM_INPUT] Routing event type %u to window %u\n", 
           event->type, input_mgr->focused_window_id);
    }

    return 0;
}

int stlxdm_input_manager_set_focus(stlxdm_input_manager_t* input_mgr, stlxdm_client_info_t* client) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }

    stlxdm_client_info_t* old_client = input_mgr->focused_client;
    uint32_t old_window_id = input_mgr->focused_window_id;

    if (client) {
        input_mgr->focused_client = client;
        input_mgr->focused_window_id = client->window ? client->window->window_id : 0;
    } else {
        input_mgr->focused_client = NULL;
        input_mgr->focused_window_id = 0;
    }

    if (old_client != client) {
        input_mgr->stats.focus_changes++;
        printf("[STLXDM_INPUT] Focus changed: %u -> %u\n", old_window_id, input_mgr->focused_window_id);
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
    
    printf("[STLXDM_INPUT] Input grabbed by window %u (type: 0x%02x)\n", window_id, grab_type);
    return 0;
}

int stlxdm_input_manager_ungrab_input(stlxdm_input_manager_t* input_mgr) {
    if (!input_mgr || !input_mgr->initialized) {
        return -1;
    }
    
    printf("[STLXDM_INPUT] Input ungrabbed (was window %u)\n", input_mgr->grab_window_id);
    
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
 