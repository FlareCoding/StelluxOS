#ifndef SERIAL_TERMINAL_H
#define SERIAL_TERMINAL_H

/* The terminal the monitor's keyboard comes from, raw for the session and as found after it */

/**
 * @brief Makes stdin raw with cfmakeraw when it is a terminal, saving its mode, so Ctrl-C reaches the board and
 * Ctrl-] reaches the monitor. A pipe or a file is left alone.
 * @return 1 when it made stdin raw, 0 when stdin is not a terminal, or -1 with errno set.
 */
int terminal_enter_raw(void);

/**
 * @brief Gives stdin back the saved mode. Does nothing when nothing changed.
 */
void terminal_restore(void);

#endif /* SERIAL_TERMINAL_H */
