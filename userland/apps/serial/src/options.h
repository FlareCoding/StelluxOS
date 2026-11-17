#ifndef SERIAL_OPTIONS_H
#define SERIAL_OPTIONS_H

/* The command line of `serial`, parsed for list, watch and monitor alike, with no I/O. Every option but --events takes
 * exactly one value, so the port may come before, between or after the options */

#include "relay.h"
#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMMAND_LIST,
    COMMAND_WATCH,
    COMMAND_MONITOR,
} command_kind;

typedef struct {
    command_kind    command;
    bool            has_events;
    const char*     port;     // the monitor's port name, after PORT_DIRECTORY when a path gave it, pointing into argv
    line_settings   line;     // from line_settings_default unless options change it
    line_request    levels;   // DTR and RTS, both LEVEL_KEEP unless options change them
    line_pulse      pulse;    // PULSE_NONE unless --pulse
    uint32_t        break_ms; // 0 for no break
    session_options session;  // from session_options_default unless options change it
} serial_args;

/**
 * @brief Parses the arguments after the program name into `out`. Every option may repeat, and the last one counts.
 * @return 0, or -1 with a sentence naming the first problem in `problem`, for a usage error.
 */
int options_parse(int argc, char* const argv[], serial_args* out, char* problem, size_t problem_size);

/**
 * @brief The usage text a usage error prints after its sentence.
 */
const char* options_usage(void);

#endif /* SERIAL_OPTIONS_H */
