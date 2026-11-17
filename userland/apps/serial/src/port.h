#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

/* A port the monitor holds: opened under a shared lock, given its line settings, and its DTR, RTS, pulse and break.
 * Calls return -1 with errno set when they fail */

#include "settings.h"

#include <stddef.h>
#include <stdint.h>

#define PULSE_MS 100

typedef struct {
    int         fd;   // -1 while closed
    const char* name; // its name in PORT_DIRECTORY
} serial_port;

/**
 * @brief Opens PORT_DIRECTORY/<name> to read and write without blocking, then takes a shared flock without waiting,
 * so a flasher that locks exclusively fails at once instead of sharing the board's output. An open of a port that no
 * other file holds open applies its stored settings and raises DTR and RTS unless their speed is 0.
 * @return 0, or -1 with errno ENOENT for no such port, EBUSY or EWOULDBLOCK for one held exclusively, and EIO for
 *         one leaving or whose chip fails a request of that open.
 */
int port_open(serial_port* port);

/**
 * @brief Reads the port's settings with TCGETS2, writes `settings` into them and applies them with TCSETSF2, which
 * empties the receive queue once the chip has taken them, dropping what the port queued at the old speed. Bytes the
 * chip still holds arrive after the flush.
 * @return 0, or -1 with errno EINVAL when the port refuses a setting, EINTR when a signal arrives while it drains
 *         what was sent before, and EIO once the port has left, when a chip request fails or when that drain passes
 *         its deadline.
 */
int port_set_line(serial_port* port, const line_settings* settings);

/**
 * @brief Sets the lines `request` names, both with one TIOCMSET, or one with TIOCMBIS or TIOCMBIC, which keep the
 * other. Either way the kernel sets both lines in one chip request, so two lines given together change at once, as
 * an ESP32 board's reset circuit needs.
 */
int port_set_levels(serial_port* port, const line_request* request);

/**
 * @brief For PULSE_MS, flips the line `pulse` names away from the level TIOCMGET reads, then puts it back. `pulse`
 * must not be PULSE_NONE, and the caller must block the signals it handles, so that none cuts the pulse short.
 */
int port_pulse(serial_port* port, line_pulse pulse);

/**
 * @brief Holds a break for `ms` with TIOCSBRK and TIOCCBRK. The caller must block the signals it handles, so that
 * none cuts the break short.
 */
int port_break(serial_port* port, uint32_t ms);

/**
 * @brief Writes to `unread` how many received bytes wait to be read, from FIONREAD.
 */
int port_count_unread(const serial_port* port, size_t* unread);

void port_close(serial_port* port);

#endif /* SERIAL_PORT_H */
