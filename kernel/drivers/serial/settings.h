#ifndef STELLUX_DRIVERS_SERIAL_SETTINGS_H
#define STELLUX_DRIVERS_SERIAL_SETTINGS_H

#include "drivers/serial/serial.h"
#include "terminal/tty.h"

namespace serial {

/**
 * @brief The settings a new port starts with: raw, 9600 baud, 8 data bits, no
 * parity and 1 stop bit.
 */
terminal::termios2 default_termios();

/**
 * @brief Translates `settings` into the line settings a port asks its chip
 * for. Refuses the flags that would edit, echo, drop or pause on bytes, since
 * a port passes bytes unchanged. Refuses an input speed other than 0 or the
 * output speed, since a chip runs both directions at one speed, and BOTHER
 * with an output speed of 0, which only B0 may ask for. Every other flag has
 * no effect here.
 * @return OK, or ERR_UNSUPPORTED with `line` unchanged.
 */
int32_t translate_termios(const terminal::termios2& settings, line_settings* line);

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_SETTINGS_H
