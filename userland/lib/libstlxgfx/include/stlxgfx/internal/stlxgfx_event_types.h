#ifndef STLXGFX_EVENT_TYPES_H
#define STLXGFX_EVENT_TYPES_H

#include <stdint.h>
#include <stlibc/input/input_event.h>

// Event queue ID for system input
#define STLXGFX_INPUT_QUEUE_ID_SYSTEM   0x0001 // Handles both, kbd and pointer events

// Event types - copied from kernel input_event.h for compatibility
typedef enum input_event_type stlxgfx_input_event_type_t;

#define STLXGFX_EVT_TYPE_INVALID                EVT_TYPE_INVALID
#define STLXGFX_KBD_EVT_KEY_PRESSED             KBD_EVT_KEY_PRESSED
#define STLXGFX_KBD_EVT_KEY_RELEASED            KBD_EVT_KEY_RELEASED
#define STLXGFX_POINTER_EVT_MOUSE_MOVED         POINTER_EVT_MOUSE_MOVED
#define STLXGFX_POINTER_EVT_MOUSE_BTN_PRESSED   POINTER_EVT_MOUSE_BTN_PRESSED
#define STLXGFX_POINTER_EVT_MOUSE_BTN_RELEASED  POINTER_EVT_MOUSE_BTN_RELEASED
#define STLXGFX_POINTER_EVT_MOUSE_SCROLLED      POINTER_EVT_MOUSE_SCROLLED

// Alias for compatibility with existing code
typedef input_event_t stlxgfx_event_t;

/**
 * @brief Keyboard key pressed event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_KBD_EVT_KEY_PRESSED
    uint32_t            keycode; // Key code of the pressed key
    uint32_t            modifiers; // Modifier keys state (Ctrl, Alt, Shift, etc.)
    int32_t             ascii_char; // ASCII character representation
    int32_t             reserved;   // Reserved for future use
} __attribute__((packed)) stlxgfx_keyboard_key_pressed_event_t;

/**
 * @brief Keyboard key released event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_KBD_EVT_KEY_RELEASED
    uint32_t            keycode; // Key code of the released key
    uint32_t            modifiers; // Modifier keys state (Ctrl, Alt, Shift, etc.)
    int32_t             reserved1; // Reserved for future use
    int32_t             reserved2; // Reserved for future use
} __attribute__((packed)) stlxgfx_keyboard_key_released_event_t;

/**
 * @brief Mouse movement event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_POINTER_EVT_MOUSE_MOVED
    uint32_t            x_pos;  // Current X position of mouse cursor
    uint32_t            y_pos;  // Current Y position of mouse cursor
    int32_t             delta_x; // Change in X position since last event
    int32_t             delta_y; // Change in Y position since last event
} __attribute__((packed)) stlxgfx_pointer_mouse_moved_event_t;

/**
 * @brief Mouse button pressed event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_POINTER_EVT_MOUSE_BTN_PRESSED
    uint32_t            button; // Button that was pressed (1=left, 2=right, 3=middle, etc.)
    uint32_t            x_pos;  // X position when button was pressed
    int32_t             y_pos;  // Y position when button was pressed
    int32_t             reserved; // Reserved for future use
} __attribute__((packed)) stlxgfx_pointer_mouse_btn_pressed_event_t;

/**
 * @brief Mouse button released event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_POINTER_EVT_MOUSE_BTN_RELEASED
    uint32_t            button; // Button that was released (1=left, 2=right, 3=middle, etc.)
    uint32_t            x_pos;  // X position when button was released
    int32_t             y_pos;  // Y position when button was released
    int32_t             reserved; // Reserved for future use
} __attribute__((packed)) stlxgfx_pointer_mouse_btn_released_event_t;

/**
 * @brief Mouse scroll event structure.
 */
typedef struct {
    uint32_t            id;     // Event-specific ID
    stlxgfx_input_event_type_t type;   // Always STLXGFX_POINTER_EVT_MOUSE_SCROLLED
    uint32_t            scroll_type; // Type of scroll (0=vertical, 1=horizontal)
    uint32_t            x_pos;  // X position when scroll occurred
    int32_t             y_pos;  // Y position when scroll occurred
    int32_t             scroll_delta; // Scroll amount (positive=up/right, negative=down/left)
} __attribute__((packed)) stlxgfx_pointer_mouse_scrolled_event_t;

#endif // STLXGFX_EVENT_TYPES_H
