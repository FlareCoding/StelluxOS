#ifndef SERIAL_LIST_H
#define SERIAL_LIST_H

/* serial list and serial watch: the ports the listing shows, once or as they come and go */

#include "options.h"

/**
 * @brief Reports every port once.
 * @return 0, or 1 when the listing cannot be read.
 */
int run_list(const serial_args* args);

/**
 * @brief Reports every port, then each arrival and departure until a signal ends the program.
 * @return 1, since it returns only when the listing cannot be read.
 */
int run_watch(const serial_args* args);

#endif /* SERIAL_LIST_H */
