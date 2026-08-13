#ifndef STELLUX_TERMINAL_TERMINAL_H
#define STELLUX_TERMINAL_TERMINAL_H

#include "common/types.h"

struct ring_buffer;
namespace resource { struct resource_ops; }

namespace terminal {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

// Stellux-internal line-discipline mode-switch ioctls.
// Encoded as _IO('s', N) so they do not collide with Linux's tty ioctl
constexpr uint32_t STLX_TCSETS_RAW    = 0x7301;
constexpr uint32_t STLX_TCSETS_COOKED = 0x7302;

// Linux tty ioctls for the terminal foreground process group
constexpr uint32_t TIOCGPGRP = 0x540F;
constexpr uint32_t TIOCSPGRP = 0x5410;

/**
 * @brief Initialize the global console terminal. Creates the input ring
 * buffer, registers as the serial RX callback, enables serial RX
 * interrupts, and creates /dev/console in devfs.
 * Must be called after both irq::init() and fs::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Feed a character into the terminal's line discipline.
 * Matches serial::rx_callback_t signature. Called from ISR context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void input_char(char c);

/**
 * @brief Get the console terminal's input ring buffer.
 * Callers must be elevated to access the returned pointer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ring_buffer* console_input_rb();

/**
 * @brief Switch the console terminal between raw and cooked mode.
 * Elevates internally for the spinlock critical section.
 * @param cmd STLX_TCSETS_RAW or STLX_TCSETS_COOKED.
 * @return OK on success, ERR on invalid cmd.
 */
int32_t set_mode(uint32_t cmd);

/**
 * @brief Console terminal ioctl handling: the foreground process group
 * ioctls plus the mode-switch commands set_mode accepts.
 * @return OK on success, ERR on unsupported cmd or bad argument.
 */
int32_t console_ioctl(uint32_t cmd, uint64_t arg);

/**
 * @brief Get the terminal resource ops table for creating resource_objects.
 */
const resource::resource_ops* get_terminal_ops();

} // namespace terminal

#endif // STELLUX_TERMINAL_TERMINAL_H
