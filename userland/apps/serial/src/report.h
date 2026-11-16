#ifndef SERIAL_REPORT_H
#define SERIAL_REPORT_H

/* What each command tells its caller: sentences for a person, or with --events tab-separated lines for a program.
 * Each line is written out as soon as it is complete */

#include "listing.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    FAILURE_IO,
} failure_kind;

/**
 * @brief Sends every later report to `out`, and must be called before the first. list and watch give stdout.
 */
void report_use(FILE* out, bool has_events);

void report_ports(const listing* ports);      // list, and watch at its start: each port, and with --events their count
void report_added(const listed_port* port);   // watch
void report_removed(const listed_port* port); // watch

/**
 * @brief Reports why a command failed: a sentence on stderr, or with --events a failed line on the report stream
 * naming `subject`, a port's name or "-" for the whole command.
 * @return 1, the exit status.
 */
int report_failure(const char* subject, failure_kind kind, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* SERIAL_REPORT_H */
