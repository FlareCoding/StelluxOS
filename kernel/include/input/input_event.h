#ifndef INPUT_EVENT_H
#define INPUT_EVENT_H
#include <types.h>

#define INPUT_QUEUE_ID_KBD      0x0001
#define INPUT_QUEUE_ID_POINTER  0x0002

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
} // namespace input

#endif // INPUT_EVENT_H

