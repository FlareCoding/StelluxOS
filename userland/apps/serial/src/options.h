#ifndef SERIAL_OPTIONS_H
#define SERIAL_OPTIONS_H

/* The command line of `serial`, parsed with no I/O */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMMAND_LIST,
    COMMAND_WATCH,
} command_kind;

typedef struct {
    command_kind command;
    bool         has_events;
} serial_args;

/**
 * @brief Parses the arguments after the program name into `out`.
 * @return 0, or -1 with a sentence naming the first problem in `problem`, for a usage error.
 */
int options_parse(int argc, char* const argv[], serial_args* out, char* problem, size_t problem_size);

/**
 * @brief The usage text a usage error prints after its sentence.
 */
const char* options_usage(void);

#endif /* SERIAL_OPTIONS_H */
