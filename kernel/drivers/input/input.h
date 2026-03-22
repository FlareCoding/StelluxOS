#ifndef STELLUX_DRIVERS_INPUT_INPUT_H
#define STELLUX_DRIVERS_INPUT_INPUT_H

#include "common/types.h"

namespace input {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

constexpr uint8_t KBD_ACTION_DOWN = 0;
constexpr uint8_t KBD_ACTION_UP   = 1;

constexpr uint8_t MOUSE_FLAG_RELATIVE = (1u << 0);

struct kbd_event {
    uint8_t  action;
    uint8_t  modifiers;
    uint16_t usage;
    uint8_t  reserved[4];
} __attribute__((packed));

struct mouse_event {
    int32_t  x_value;
    int32_t  y_value;
    int16_t  wheel;
    uint16_t buttons;
    uint8_t  flags;
    uint8_t  reserved[3];
} __attribute__((packed));

static_assert(sizeof(kbd_event) == 8, "kbd_event must be 8 bytes");
static_assert(sizeof(mouse_event) == 16, "mouse_event must be 16 bytes");

/**
 * @brief Initialize the input subsystem and register /dev/input/kbd
 *        and /dev/input/mouse.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Enqueue a keyboard event. Non-blocking; drops on overflow.
 * Internally elevates privilege to access the ring buffer.
 * @return Number of events enqueued (1) or 0 on overflow.
 * @note Safe to call from any kernel context including user-mode tasks.
 */
int32_t push_kbd_event(const kbd_event& evt);

/**
 * @brief Enqueue a mouse event. Non-blocking; drops on overflow.
 * Internally elevates privilege to access the ring buffer.
 * @return Number of events enqueued (1) or 0 on overflow.
 * @note Safe to call from any kernel context including user-mode tasks.
 */
int32_t push_mouse_event(const mouse_event& evt);

} // namespace input

#endif // STELLUX_DRIVERS_INPUT_INPUT_H
