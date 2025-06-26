#ifndef STLIBC_INPUT_EVT_H
#define STLIBC_INPUT_EVT_H

#include <stlibc/stlibcdef.h>

// Input queue identifiers (must match kernel definitions)
#define INPUT_QUEUE_ID_SYSTEM   0x0001  // System input queue for keyboard and pointer events

enum input_event_type {
    EVT_TYPE_INVALID = 0x0,

    KBD_EVT_KEY_PRESSED, KBD_EVT_KEY_RELEASED,

    POINTER_EVT_MOUSE_MOVED, POINTER_EVT_MOUSE_BTN_PRESSED,
    POINTER_EVT_MOUSE_BTN_RELEASED, POINTER_EVT_MOUSE_SCROLLED
};

/** 
 * @brief Represents an input event structure.
 */
typedef struct input_event {
    uint32_t                id;     // Event-specific ID     
    enum input_event_type   type;   // Event type (e.g., key press, mouse movement)
    uint32_t                udata1; // Event-specific unsigned data 1
    uint32_t                udata2; // Event-specific unsigned data 2
    int32_t                 sdata1; // Event-specific signed data 1
    int32_t                 sdata2; // Event-specific signed data 2
} __attribute__((packed)) input_event_t;

#endif // STLIBC_INPUT_EVT_H
