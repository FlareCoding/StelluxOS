#ifndef STLX_INPUT_H
#define STLX_INPUT_H

#include <stdint.h>

#define STLX_INPUT_KBD_ACTION_DOWN  0
#define STLX_INPUT_KBD_ACTION_UP    1

#define STLX_INPUT_MOUSE_FLAG_RELATIVE  (1u << 0)

#define STLX_INPUT_MOD_LCTRL   (1u << 0)
#define STLX_INPUT_MOD_LSHIFT  (1u << 1)
#define STLX_INPUT_MOD_LALT    (1u << 2)
#define STLX_INPUT_MOD_LGUI    (1u << 3)
#define STLX_INPUT_MOD_RCTRL   (1u << 4)
#define STLX_INPUT_MOD_RSHIFT  (1u << 5)
#define STLX_INPUT_MOD_RALT    (1u << 6)
#define STLX_INPUT_MOD_RGUI    (1u << 7)

typedef struct {
    uint8_t  action;
    uint8_t  modifiers;
    uint16_t usage;
    uint8_t  reserved[4];
} __attribute__((packed)) stlx_input_kbd_event_t;

typedef struct {
    int32_t  x_value;
    int32_t  y_value;
    int16_t  wheel;
    uint16_t buttons;
    uint8_t  flags;
    uint8_t  reserved[3];
} __attribute__((packed)) stlx_input_mouse_event_t;

#endif /* STLX_INPUT_H */
