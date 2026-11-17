#ifndef SERIAL_REPORT_H
#define SERIAL_REPORT_H

/* What each command tells its caller: sentences for a person, or with --events tab-separated lines for a program.
 * Each line is written out as soon as it is complete */

#include "listing.h"
#include "settings.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    FAILURE_MISSING,
    FAILURE_BUSY,
    FAILURE_SETTINGS,
    FAILURE_IO,
} failure_kind;

/**
 * @brief Sends every later report to `out`, and must be called before the first. list and watch give stdout, and
 * the monitor stderr, since its stdout carries the board's bytes. Events end in LF alone, since a CR would become
 * part of a program's last field, and sentences end in CR LF when `is_raw_terminal`, since a raw terminal does not
 * return on LF.
 */
void report_use(FILE* out, bool has_events, bool is_raw_terminal);

void report_ports(const listing* ports);      // list, and watch at its start: each port, and with --events their count
void report_added(const listed_port* port);   // watch
void report_removed(const listed_port* port); // watch

void report_open(const char* name, const line_settings* settings);
void report_gone(const char* name, int error); // error 0 when the board left, else the errno its read failed with
void report_back(const char* name);
void report_dropping(const char* name);        // a terminal's typed bytes are dropped, a sentence only
void report_closed(const char* name);

/**
 * @brief Reports why a command failed: a sentence on stderr, or with --events a failed line on the report stream
 * naming `subject`, a port's name or "-" for the whole command.
 * @return 1, the exit status.
 */
int report_failure(const char* subject, failure_kind kind, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief The failure kind for `error`: missing for ENOENT, busy for EBUSY and EWOULDBLOCK, settings for EINVAL, io
 * for any other.
 */
failure_kind failure_of_errno(int error);

#endif /* SERIAL_REPORT_H */
