#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EVENTS_FLAG "--events"

static const char USAGE[] =
    "Usage: serial list [--events]\n"
    "       serial watch [--events]\n"
    "\n"
    "--events prints tab-separated facts instead of sentences.\n";

static __attribute__((format(printf, 3, 4))) int refuse(char* problem, size_t problem_size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(problem, problem_size, format, args);
    va_end(args);

    return -1;
}

int options_parse(int argc, char* const argv[], serial_args* out, char* problem, size_t problem_size) {
    if (argc < 1) {
        return refuse(problem, problem_size, "the command is missing");
    }

    const char* command = argv[0];
    if (strcmp(command, "list") == 0) {
        out->command = COMMAND_LIST;
    } else if (strcmp(command, "watch") == 0) {
        out->command = COMMAND_WATCH;
    } else {
        return refuse(problem, problem_size, "\"%s\" is not a command", command);
    }

    out->has_events = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], EVENTS_FLAG) != 0) {
            return refuse(problem, problem_size, "%s takes only %s, not \"%s\"", command, EVENTS_FLAG, argv[i]);
        }

        out->has_events = true;
    }

    return 0;
}

const char* options_usage(void) {
    return USAGE;
}
