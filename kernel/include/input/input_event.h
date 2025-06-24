#ifndef INPUT_EVENT_H
#define INPUT_EVENT_H
#include <types.h>

#define INPUT_QUEUE_ID_SYSTEM   0x0001 // Handles both, kbd and pointer events

namespace input {
enum input_event_type : uint32_t {
    EVT_TYPE_INVALID = 0x0,

    KBD_EVT_KEY_PRESSED, KBD_EVT_KEY_RELEASED,

    POINTER_EVT_MOUSE_MOVED, POINTER_EVT_MOUSE_BTN_PRESSED,
    POINTER_EVT_MOUSE_BTN_RELEASED, POINTER_EVT_MOUSE_SCROLLED
};

/** 
 * @brief Represents an input event structure.
 */
struct input_event_t {
    uint32_t            id;     // Event-specific ID     
    input_event_type    type;   // Event type (e.g., key press, mouse movement)
    uint32_t            udata1; // Event-specific unsigned data 1
    uint32_t            udata2; // Event-specific unsigned data 2
    int32_t             sdata1; // Event-specific signed data 1
    int32_t             sdata2; // Event-specific signed data 2
} __attribute__((packed));

/**
 * @brief Keyboard key pressed event structure.
 */
struct keyboard_key_pressed_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always KBD_EVT_KEY_PRESSED
    uint32_t            keycode; // Key code of the pressed key
    uint32_t            modifiers; // Modifier keys state (Ctrl, Alt, Shift, etc.)
    int32_t             ascii_char; // ASCII character representation
    int32_t             reserved;   // Reserved for future use
} __attribute__((packed));

/**
 * @brief Keyboard key released event structure.
 */
struct keyboard_key_released_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always KBD_EVT_KEY_RELEASED
    uint32_t            keycode; // Key code of the released key
    uint32_t            modifiers; // Modifier keys state (Ctrl, Alt, Shift, etc.)
    int32_t             reserved1; // Reserved for future use
    int32_t             reserved2; // Reserved for future use
} __attribute__((packed));

/**
 * @brief Mouse movement event structure.
 */
struct pointer_mouse_moved_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always POINTER_EVT_MOUSE_MOVED
    uint32_t            x_pos;  // Current X position of mouse cursor
    uint32_t            y_pos;  // Current Y position of mouse cursor
    int32_t             delta_x; // Change in X position since last event
    int32_t             delta_y; // Change in Y position since last event
} __attribute__((packed));

/**
 * @brief Mouse button pressed event structure.
 */
struct pointer_mouse_btn_pressed_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always POINTER_EVT_MOUSE_BTN_PRESSED
    uint32_t            button; // Button that was pressed (1=left, 2=right, 3=middle, etc.)
    uint32_t            x_pos;  // X position when button was pressed
    int32_t             y_pos;  // Y position when button was pressed
    int32_t             reserved; // Reserved for future use
} __attribute__((packed));

/**
 * @brief Mouse button released event structure.
 */
struct pointer_mouse_btn_released_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always POINTER_EVT_MOUSE_BTN_RELEASED
    uint32_t            button; // Button that was released (1=left, 2=right, 3=middle, etc.)
    uint32_t            x_pos;  // X position when button was released
    int32_t             y_pos;  // Y position when button was released
    int32_t             reserved; // Reserved for future use
} __attribute__((packed));

/**
 * @brief Mouse scroll event structure.
 */
struct pointer_mouse_scrolled_event_t {
    uint32_t            id;     // Event-specific ID
    input_event_type    type;   // Always POINTER_EVT_MOUSE_SCROLLED
    uint32_t            scroll_type; // Type of scroll (0=vertical, 1=horizontal)
    uint32_t            x_pos;  // X position when scroll occurred
    int32_t             y_pos;  // Y position when scroll occurred
    int32_t             scroll_delta; // Scroll amount (positive=up/right, negative=down/left)
} __attribute__((packed));
} // namespace input

#endif // INPUT_EVENT_H

