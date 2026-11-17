#ifndef SERIAL_MONITOR_H
#define SERIAL_MONITOR_H

/* serial monitor: connects the terminal to a port until Ctrl-], and reopens it when the board comes back */

#include "options.h"

/**
 * @brief Runs a session on the port `args` names. The port is read only as fast as stdout takes the board's bytes, so a
 * reader that stops leaves them in the port's queue, and Ctrl-] and the ending signals still end the session.
 * @return 0 for a session that ended by Ctrl-], SIGINT, SIGTERM, SIGHUP or a closed stdout, or 1 for a failure.
 */
int run_monitor(const serial_args* args);

#endif /* SERIAL_MONITOR_H */
